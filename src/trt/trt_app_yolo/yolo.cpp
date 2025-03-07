#if USE_TRT



#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>

#include "../../../include/private/airuntime/logger.h"
#include "../../../include/public/AIRuntimeUtils.h"

#include "../../../include/private/trt/trt_common/cuda-tools.hpp"
#include "../../../include/private/trt/trt_common/monopoly_allocator.hpp"
#include "../../../include/private/trt/trt_common/time_cost.h"
#include "../../../include/private/trt/trt_common/trt_infer.hpp"
#include "../../../include/private/trt/trt_common/trt_infer_schedule.hpp"
#include "../../../include/private/trt/trt_cuda/preprocess_kernel.cuh"

// #include "algo/algo_interface.h"
#include "../../../include/private/trt/trt_app_yolo/yolo.hpp"

namespace Yolo {
using namespace cv;
using namespace std;
using BoxArray = Algo::BoxArray;
using Box      = Algo::Box;

const char* type_name(Type type)
{
    switch (type) {
        case Type::V5:
            return "YoloV5";
        case Type::X:
            return "YoloX";
        default:
            return "Unknow";
    }
}

void decode_kernel_invoker(float* predict, int num_bboxes, int num_classes, float confidence_threshold, float* invert_affine_matrix, float* parray, int max_objects, cudaStream_t stream);

void nms_kernel_invoker(float* parray, float nms_threshold, int max_objects, cudaStream_t stream);

struct AffineMatrix
{
    float i2d[6];  // image to dst(network), 2x3 matrix
    float d2i[6];  // dst to image, 2x3 matrix

    void compute(const cv::Size& from, const cv::Size& to)
    {
        float scale_x = to.width / (float)from.width;
        float scale_y = to.height / (float)from.height;
        float scale   = std::min(scale_x, scale_y);
        i2d[0]        = scale;
        i2d[1]        = 0;
        i2d[2]        = -scale * from.width * 0.5 + to.width * 0.5 + scale * 0.5 - 0.5;
        i2d[3]        = 0;
        i2d[4]        = scale;
        i2d[5]        = -scale * from.height * 0.5 + to.height * 0.5 + scale * 0.5 - 0.5;

        cv::Mat m2x3_i2d(2, 3, CV_32F, i2d);
        cv::Mat m2x3_d2i(2, 3, CV_32F, d2i);
        cv::invertAffineTransform(m2x3_i2d, m2x3_d2i);
    }

    cv::Mat i2d_mat() { return cv::Mat(2, 3, CV_32F, i2d); }
};

static float iou(const Box& a, const Box& b)
{
    float cleft   = max(a.left, b.left);
    float ctop    = max(a.top, b.top);
    float cright  = min(a.right, b.right);
    float cbottom = min(a.bottom, b.bottom);

    float c_area = max(cright - cleft, 0.0f) * max(cbottom - ctop, 0.0f);
    if (c_area == 0.0f)
        return 0.0f;

    float a_area = max(0.0f, a.right - a.left) * max(0.0f, a.bottom - a.top);
    float b_area = max(0.0f, b.right - b.left) * max(0.0f, b.bottom - b.top);
    return c_area / (a_area + b_area - c_area);
}

static BoxArray cpu_nms(BoxArray& boxes, float threshold)
{
    std::sort(boxes.begin(), boxes.end(), [](BoxArray::const_reference a, BoxArray::const_reference b) {
        return a.confidence > b.confidence;
    });

    BoxArray output;
    output.reserve(boxes.size());

    vector<bool> remove_flags(boxes.size());
    for (int i = 0; i < boxes.size(); ++i) {
        if (remove_flags[i])
            continue;

        auto& a = boxes[i];
        output.emplace_back(a);

        for (int j = i + 1; j < boxes.size(); ++j) {
            if (remove_flags[j])
                continue;

            auto& b = boxes[j];
            if (b.class_label == a.class_label) {
                if (iou(a, b) >= threshold)
                    remove_flags[j] = true;
            }
        }
    }
    return output;
}

using ControllerImpl = InferController<Mat,                 // input
                                       BoxArray,            // output
                                       tuple<string, int>,  // start param
                                       AffineMatrix         // additional
                                       >;
class InferImpl : public Infer, public ControllerImpl
{
public:
    virtual ~InferImpl() { stop(); }

    virtual bool startup(const string& file, Type type, int gpuid, float confidence_threshold, float nms_threshold, NMSMethod nms_method, int max_objects, bool use_multi_preprocess_stream)
    {
        if (type == Type::V5) {
            normalize_ = CUDAKernel::Norm::alpha_beta(
                1 / 255.0f, 0.0f, CUDAKernel::ChannelType::Invert);
        }
        else if (type == Type::X) {
            // float mean[] = {0.485, 0.456, 0.406};
            // float std[]  = {0.229, 0.224, 0.225};
            // normalize_ = CUDAKernel::Norm::mean_std(mean, std, 1/255.0f,
            // CUDAKernel::ChannelType::Invert);
            normalize_ = CUDAKernel::Norm::None();
        }
        
        else {
            INFOE("Unsupport type %d", type);
        }

        use_multi_preprocess_stream_ = use_multi_preprocess_stream;
        confidence_threshold_        = confidence_threshold;
        nms_threshold_               = nms_threshold;
        nms_method_                  = nms_method;
        max_objects_                 = max_objects;
        return ControllerImpl::startup(make_tuple(file, gpuid));
    }

    virtual bool set_param(const json& config) override
    {
        confidence_threshold_ =
            get_param<float>(config, "confidence_threshold", confidence_threshold_);
        nms_threshold_ = get_param<float>(config, "nms_threshold", nms_threshold_);
        max_objects_   = get_param<float>(config, "max_objects", max_objects_);
        return true;
    }

    virtual void worker(promise<bool>& result) override
    {
        string file  = get<0>(start_param_);
        int    gpuid = get<1>(start_param_);

        TRT::set_device(gpuid);
        auto engine = TRT::load_infer(file);
        if (engine == nullptr) {
            LOG_INFOE("Engine {} load failed", file.c_str());
            result.set_value(false);
            return;
        }

        engine->print();
        const int MAX_IMAGE_BBOX   = max_objects_;
        const int NUM_BOX_ELEMENT =
            7;  // left, top, right, bottom, confidence, class, keepflag
        TRT::Tensor affin_matrix_device(TRT::DataType::Float);
        TRT::Tensor output_array_device(TRT::DataType::Float);
        int         max_batch_size = engine->get_max_batch_size();
        auto        input          = engine->tensor(engine->get_input_name(0));  //"images"
        //auto        output         = engine->tensor("output");// yolo5
        auto output      = engine->tensor(engine->get_output_name(0));//"output"
        int         num_classes    = output->size(2) - 5;

        input_width_  = input->size(3);
        input_height_ = input->size(2);
        model_info_["memory_size"] = engine->get_device_memory_size() >> 20;
        model_info_["dims"] = {input->size(0), input->size(1), input->size(2), input->size(3)};

        tensor_allocator_ =
            make_shared<MonopolyAllocator<TRT::Tensor>>(max_batch_size * 2);
        stream_ = engine->get_stream();
        gpu_    = gpuid;
        result.set_value(true);

        input->resize_single_dim(0, max_batch_size).to_gpu();
        affin_matrix_device.set_stream(stream_);

        affin_matrix_device.resize(max_batch_size, 8).to_gpu();

        output_array_device
            .resize(max_batch_size, 1 + MAX_IMAGE_BBOX * NUM_BOX_ELEMENT)
            .to_gpu();

        vector<Job> fetch_jobs;
        while (get_jobs_and_wait(fetch_jobs, max_batch_size)) {
            int infer_batch_size = fetch_jobs.size();
            input->resize_single_dim(0, infer_batch_size);

            for (int ibatch = 0; ibatch < infer_batch_size; ++ibatch) {
                auto& job  = fetch_jobs[ibatch];
                auto& mono = job.mono_tensor->data();

                if (mono->get_stream() != stream_) {
                    // synchronize preprocess stream finish
                    checkCudaRuntime(cudaStreamSynchronize(mono->get_stream()));
                }

                affin_matrix_device.copy_from_gpu(affin_matrix_device.offset(ibatch), mono->get_workspace()->gpu(), 6);
                input->copy_from_gpu(input->offset(ibatch), mono->gpu(), mono->count());
                job.mono_tensor->release();
            }
            TRT::TimeCost infer_time_cost;
            infer_time_cost.start();
            engine->forward();
            infer_time_cost.stop();

            // INFO("Inference Cost Time : %lld ms", infer_time_cost.get_cost_time());
            output_array_device.to_gpu(false);
            TRT::TimeCost post_time_cost;
            post_time_cost.start();
            for (int ibatch = 0; ibatch < infer_batch_size; ++ibatch) {
                auto&  job                = fetch_jobs[ibatch];
                float* image_based_output = output->gpu<float>(ibatch);
                float* output_array_ptr   = output_array_device.gpu<float>(ibatch);
                auto   affine_matrix      = affin_matrix_device.gpu<float>(ibatch);
                checkCudaRuntime(
                    cudaMemsetAsync(output_array_ptr, 0, sizeof(int), stream_));
                decode_kernel_invoker(image_based_output, output->size(1), num_classes, confidence_threshold_, affine_matrix, output_array_ptr, MAX_IMAGE_BBOX, stream_);
                if (nms_method_ == NMSMethod::FastGPU) {
                    nms_kernel_invoker(output_array_ptr, nms_threshold_, MAX_IMAGE_BBOX, stream_);
                }
            }
            post_time_cost.stop();
            output_array_device.to_cpu();
            for (int ibatch = 0; ibatch < infer_batch_size; ++ibatch) {

                float* parray            = output_array_device.cpu<float>(ibatch);
                int    count             = min(MAX_IMAGE_BBOX, (int)*parray);
                auto&  job               = fetch_jobs[ibatch];
                auto&  image_based_boxes = job.output;

                for (int i = 0; i < count; ++i) {
                    float* pbox     = parray + 1 + i * NUM_BOX_ELEMENT;
                    int    label    = pbox[5];
                    int    keepflag = pbox[6];
                    if (keepflag == 1) {
                        Box box(pbox[0], pbox[1], pbox[2], pbox[3], pbox[4], label);
                        image_based_boxes.emplace_back(box);
                    }
                }
                image_based_boxes.pre_time =
                    job.preTime.get_cost_time() / infer_batch_size;
                image_based_boxes.infer_time =
                    infer_time_cost.get_cost_time() / infer_batch_size;
                image_based_boxes.host_time =
                    post_time_cost.get_cost_time() / infer_batch_size;
                image_based_boxes.total_time = image_based_boxes.pre_time +
                                               image_based_boxes.infer_time +
                                               image_based_boxes.host_time;
                model_info_["infer_time"] = image_based_boxes.total_time;
                if (nms_method_ == NMSMethod::CPU) {
                    image_based_boxes = cpu_nms(image_based_boxes, nms_threshold_);
                }
                job.pro->set_value(image_based_boxes);
            }
            fetch_jobs.clear();
        }
        stream_ = nullptr;
        tensor_allocator_.reset();
        INFO("Engine destroy.");
    }

    virtual bool preprocess(Job& job, const Mat& image) override
    {
        job.preTime.start();
        if (tensor_allocator_ == nullptr) {
            LOG_INFOE("tensor_allocator_ is nullptr");
            return false;
        }

        if (image.empty()) {
            LOG_INFOE("Image is empty");
            return false;
        }

        job.mono_tensor = tensor_allocator_->query();
        if (job.mono_tensor == nullptr) {
            LOG_INFOE("Tensor allocator query failed.");
            return false;
        }

        CUDATools::AutoDevice auto_device(gpu_);
        auto&                 tensor            = job.mono_tensor->data();
        TRT::CUStream         preprocess_stream = nullptr;
        TRT::TimeCost         pre_time_cost;
        pre_time_cost.start();
        if (tensor == nullptr) {
            // not init
            tensor = make_shared<TRT::Tensor>();
            tensor->set_workspace(make_shared<TRT::MixMemory>());

            if (use_multi_preprocess_stream_) {
                checkCudaRuntime(cudaStreamCreate(&preprocess_stream));

                // owner = true, stream needs to be free during deconstruction
                tensor->set_stream(preprocess_stream, true);
            }
            else {
                preprocess_stream = stream_;

                // owner = false, tensor ignored the stream
                tensor->set_stream(preprocess_stream, false);
            }
        }

        Size input_size(input_width_, input_height_);
        job.additional.compute(image.size(), input_size);

        preprocess_stream = tensor->get_stream();
        tensor->resize(1, 3, input_height_, input_width_);

        size_t   size_image  = image.cols * image.rows * image.channels();
        size_t   size_matrix = iLogger::upbound(sizeof(job.additional.d2i), 32);
        auto     workspace   = tensor->get_workspace();
        uint8_t* gpu_workspace =
            (uint8_t*)workspace->gpu(size_matrix + size_image);
        float*   affine_matrix_device = (float*)gpu_workspace;
        uint8_t* image_device         = size_matrix + gpu_workspace;

        uint8_t* cpu_workspace =
            (uint8_t*)workspace->cpu(size_matrix + size_image);
        float*   affine_matrix_host = (float*)cpu_workspace;
        uint8_t* image_host         = size_matrix + cpu_workspace;

        // checkCudaRuntime(cudaMemcpyAsync(image_host,   image.data, size_image,
        // cudaMemcpyHostToHost,   stream_));
        //  speed up
        memcpy(image_host, image.data, size_image);
        memcpy(affine_matrix_host, job.additional.d2i, sizeof(job.additional.d2i));
        checkCudaRuntime(cudaMemcpyAsync(image_device, image_host, size_image, cudaMemcpyHostToDevice, preprocess_stream));
        checkCudaRuntime(cudaMemcpyAsync(
            affine_matrix_device, affine_matrix_host, sizeof(job.additional.d2i),
            cudaMemcpyHostToDevice, preprocess_stream));

        CUDAKernel::warp_affine_bilinear_and_normalize_plane(
            image_device, image.cols * 3, image.cols, image.rows,
            tensor->gpu<float>(), input_width_, input_height_, affine_matrix_device,
            114, normalize_, preprocess_stream);
        job.preTime.stop();
        // INFO("PreProcess Cost Time : %lld ms", pre_time_cost.get_cost_time());
        return true;
    }

    virtual vector<shared_future<BoxArray>>
    commits(const vector<Mat>& images) override
    {
        return ControllerImpl::commits(images);
    }

    virtual std::shared_future<BoxArray> commit(const Mat& image) override
    {
        return ControllerImpl::commit(image);
    }

    virtual json infer_info() override { return model_info_; }

private:
    int              input_width_                 = 0;
    int              input_height_                = 0;
    int              gpu_                         = 0;
    float            confidence_threshold_        = 0;
    float            nms_threshold_               = 0;
    int              max_objects_                 = 1024;
    NMSMethod        nms_method_                  = NMSMethod::FastGPU;
    TRT::CUStream    stream_                      = nullptr;
    bool             use_multi_preprocess_stream_ = false;
    CUDAKernel::Norm normalize_;
    json             model_info_;  // The model memory and time cost info
};

shared_ptr<Infer> create_infer(const string& engine_file, Type type, int gpuid, float confidence_threshold, float nms_threshold, NMSMethod nms_method, int max_objects, bool use_multi_preprocess_stream)
{
    shared_ptr<InferImpl> instance(new InferImpl());
    if (!instance->startup(engine_file, type, gpuid, confidence_threshold, nms_threshold, nms_method, max_objects, use_multi_preprocess_stream)) {
        instance.reset();
    }
    return instance;
}

void image_to_tensor(const cv::Mat& image, shared_ptr<TRT::Tensor>& tensor, Type type, int ibatch)
{
    CUDAKernel::Norm normalize;
    if (type == Type::V5) {
        normalize = CUDAKernel::Norm::alpha_beta(1 / 255.0f, 0.0f, CUDAKernel::ChannelType::Invert);
    }
    else if (type == Type::X) {
        // float mean[] = {0.485, 0.456, 0.406};
        // float std[]  = {0.229, 0.224, 0.225};
        // normalize_ = CUDAKernel::Norm::mean_std(mean, std, 1/255.0f,
        // CUDAKernel::ChannelType::Invert);
        normalize = CUDAKernel::Norm::None();
    }
    else {
        INFOE("Unsupport type %d", type);
    }

    Size         input_size(tensor->size(3), tensor->size(2));
    AffineMatrix affine;
    affine.compute(image.size(), input_size);

    size_t   size_image           = image.cols * image.rows * 3;
    size_t   size_matrix          = iLogger::upbound(sizeof(affine.d2i), 32);
    auto     workspace            = tensor->get_workspace();
    uint8_t* gpu_workspace        = (uint8_t*)workspace->gpu(size_matrix + size_image);
    float*   affine_matrix_device = (float*)gpu_workspace;
    uint8_t* image_device         = size_matrix + gpu_workspace;

    uint8_t* cpu_workspace      = (uint8_t*)workspace->cpu(size_matrix + size_image);
    float*   affine_matrix_host = (float*)cpu_workspace;
    uint8_t* image_host         = size_matrix + cpu_workspace;
    auto     stream             = tensor->get_stream();

    memcpy(image_host, image.data, size_image);
    memcpy(affine_matrix_host, affine.d2i, sizeof(affine.d2i));
    checkCudaRuntime(cudaMemcpyAsync(image_device, image_host, size_image, cudaMemcpyHostToDevice, stream));
    checkCudaRuntime(cudaMemcpyAsync(affine_matrix_device, affine_matrix_host, sizeof(affine.d2i), cudaMemcpyHostToDevice, stream));

    CUDAKernel::warp_affine_bilinear_and_normalize_plane(
        image_device, image.cols * 3, image.cols, image.rows,
        tensor->gpu<float>(ibatch), input_size.width, input_size.height,
        affine_matrix_device, 114, normalize, stream);
    tensor->synchronize();
}
};  // namespace Yolo



#endif //USE_TRT