// Copyright Ross Girshick and Yangqing Jia 2013
//
// matcaffe.cpp provides a wrapper of the caffe::Net class as well as some
// caffe::Caffe functions so that one could easily call it from matlab.
// Note that for matlab, we will simply use float as the data type.

#include "mex.h"
#include "caffe/caffe.hpp"

#define MEX_ARGS int nlhs, mxArray **plhs, int nrhs, const mxArray **prhs

using namespace caffe;

// The pointer to the internal caffe::Net instance
static vector<shared_ptr<Solver<float> > > solver_;
static int net_id_;
// Five things to be aware of:
//   caffe uses row-major order
//   matlab uses column-major order
//   caffe uses BGR color channel order
//   matlab uses RGB color channel order
//   images need to have the data mean subtracted
//
// Data coming in from matlab needs to be in the order 
//   [batch_images, channels, height, width] 
// where width is the fastest dimension.
// Here is the rough matlab for putting image data into the correct
// format:
//   % convert from uint8 to single
//   im = single(im);
//   % reshape to a fixed size (e.g., 227x227)
//   im = imresize(im, [IMAGE_DIM IMAGE_DIM], 'bilinear');
//   % permute from RGB to BGR and subtract the data mean (already in BGR)
//   im = im(:,:,[3 2 1]) - data_mean;
//   % flip width and height to make width the fastest dimension
//   im = permute(im, [2 1 3]);
//
// If you have multiple images, cat them with cat(4, ...)
//
// The actual forward function. It takes in a cell array of 4-D arrays as
// input and outputs a cell array. 
static mxArray* do_forward(const mxArray* const bottom) {
  vector<Blob<float>*>& input_blobs = solver_[net_id_]->net()->input_blobs();
  CHECK_EQ(static_cast<unsigned int>(mxGetDimensions(bottom)[0]), 
      input_blobs.size());
  for (unsigned int i = 0; i < input_blobs.size(); ++i) {
    const mxArray* const elem = mxGetCell(bottom, i);
    const float* const data_ptr = 
        reinterpret_cast<const float* const>(mxGetPr(elem));
    switch (Caffe::mode()) {
    case Caffe::CPU:
      memcpy(input_blobs[i]->mutable_cpu_data(), data_ptr,
          sizeof(float) * input_blobs[i]->count());
      break;
    case Caffe::GPU:
      cudaMemcpy(input_blobs[i]->mutable_gpu_data(), data_ptr,
          sizeof(float) * input_blobs[i]->count(), cudaMemcpyHostToDevice);
      break;
    default:
      LOG(FATAL) << "Unknown Caffe mode.";
    }  // switch (Caffe::mode())
  }
  const vector<Blob<float>*>& output_blobs = solver_[net_id_]->net()->ForwardPrefilled();
  mxArray* mx_out = mxCreateCellMatrix(output_blobs.size(), 1);
  for (unsigned int i = 0; i < output_blobs.size(); ++i) {
    mxArray* mx_blob = mxCreateNumericMatrix(output_blobs[i]->count(), 
        1, mxSINGLE_CLASS, mxREAL);
    mxSetCell(mx_out, i, mx_blob);
    float* data_ptr = reinterpret_cast<float*>(mxGetPr(mx_blob));
    switch (Caffe::mode()) {
    case Caffe::CPU:
      memcpy(data_ptr, output_blobs[i]->cpu_data(),
          sizeof(float) * output_blobs[i]->count());
      break;
    case Caffe::GPU:
      cudaMemcpy(data_ptr, output_blobs[i]->gpu_data(),
          sizeof(float) * output_blobs[i]->count(), cudaMemcpyDeviceToHost);
      break;
    default:
      LOG(FATAL) << "Unknown Caffe mode.";
    }  // switch (Caffe::mode())
  }

  return mx_out;
}

static mxArray* do_update(const mxArray* const bottom){
  vector<Blob<float>*>& input_blobs = solver_[net_id_]->net()->input_blobs();
  CHECK_EQ(static_cast<unsigned int>(mxGetDimensions(bottom)[0]), 
      input_blobs.size());
  for (unsigned int i = 0; i < input_blobs.size(); ++i) {
    const mxArray* const elem = mxGetCell(bottom, i);
    const float* const data_ptr = 
        reinterpret_cast<const float* const>(mxGetPr(elem));
    switch (Caffe::mode()) {
    case Caffe::CPU:
      memcpy(input_blobs[i]->mutable_cpu_data(), data_ptr,
          sizeof(float) * input_blobs[i]->count());
      break;
    case Caffe::GPU:
      CUDA_CHECK(cudaMemcpy(input_blobs[i]->mutable_gpu_data(), data_ptr,
          sizeof(float) * input_blobs[i]->count(), cudaMemcpyHostToDevice));
      break;
    default:
      LOG(FATAL) << "Unknown Caffe mode.";
    }  // switch (Caffe::mode())
  }
  double loss = solver_[net_id_]->DoUpdate();
  mxArray* mx_out = mxCreateDoubleScalar(loss);
  return mx_out;
}

// The caffe::Caffe utility functions.
static void set_mode_cpu(MEX_ARGS) { 
  Caffe::set_mode(Caffe::CPU); 
}

static void set_mode_gpu(MEX_ARGS) { 
  Caffe::set_mode(Caffe::GPU); 
}

static void set_phase_train(MEX_ARGS) {   
  Caffe::set_phase(Caffe::TRAIN); 
}

static void set_phase_test(MEX_ARGS) { 
  Caffe::set_phase(Caffe::TEST); 
}

static void set_device(MEX_ARGS) { 
  if (nrhs != 1) {
    LOG(ERROR) << "Only given " << nrhs << " arguments";
    mexErrMsgTxt("Wrong number of arguments");
  } 

  int device_id = static_cast<int>(mxGetScalar(prhs[0]));
  Caffe::SetDevice(device_id);
}

static void set_net_id(MEX_ARGS) { 
  if (nrhs != 1) {
    LOG(ERROR) << "Only given " << nrhs << " arguments";
    mexErrMsgTxt("Wrong number of arguments");
  } 

  int net_id = static_cast<int>(mxGetScalar(prhs[0]));
  net_id_ = net_id;
}

// static void init(MEX_ARGS) {
//   if (nrhs != 2) {
//     LOG(ERROR) << "Only given " << nrhs << " arguments";
//     mexErrMsgTxt("Wrong number of arguments");
//   }

//   char* param_file = mxArrayToString(prhs[0]);
//   char* model_file = mxArrayToString(prhs[1]);

//   net_.reset(new Net<float>(string(param_file)));
//   net_->CopyTrainedLayersFrom(string(model_file));

//   mxFree(param_file);
//   mxFree(model_file);
// }

static void forward(MEX_ARGS) {
  if (nrhs != 1) {
    LOG(ERROR) << "Only given " << nrhs << " arguments";
    mexErrMsgTxt("Wrong number of arguments");
  }

  plhs[0] = do_forward(prhs[0]);
}

static void update(MEX_ARGS) {
  if (nrhs != 1) {
    LOG(ERROR) << "Only given " << nrhs << " arguments";
    mexErrMsgTxt("Wrong number of arguments");
  }

  plhs[0] = do_update(prhs[0]);
}

static void set_batch_size(MEX_ARGS){
  if (nrhs != 1) {
    LOG(ERROR) << "Only given " << nrhs << " arguments";
    mexErrMsgTxt("Wrong number of arguments");
  }
  int batchSize = static_cast<int>(mxGetScalar(prhs[0]));
  solver_[net_id_]->net()->SetBatchSize(batchSize);
}

static void init_finetune(MEX_ARGS) {
  if (nrhs != 2) {
    LOG(ERROR) << "Only given " << nrhs << " arguments";
    mexErrMsgTxt("Wrong number of arguments");
  }

  char* param_file = mxArrayToString(prhs[0]);
  char* model_file = mxArrayToString(prhs[1]);
  
  SolverParameter solver_param;
  ReadProtoFromTextFile(param_file, &solver_param);

  if (solver_.size() <= net_id_) {
    solver_.push_back(shared_ptr<Solver<float> > (GetSolver<float>(solver_param)));
    solver_[net_id_]->net()->CopyTrainedLayersFrom(string(model_file));
    solver_[net_id_]->PreSolve();
  //  net_.reset(solver_->net());
  } else {
    solver_[net_id_]->net()->CopyTrainedLayersFrom(string(model_file));
    solver_[net_id_]->PreSolve();
  }
  LOG(INFO) << "Initialization Done.";

  mxFree(param_file);
  mxFree(model_file);
}

static void extract_feature(MEX_ARGS){
  if (nrhs != 1) {
    LOG(ERROR) << "Only given " << nrhs << " arguments";
    mexErrMsgTxt("Wrong number of arguments");
  }

  char* blob_name = mxArrayToString(prhs[0]);

  // Assume the data is forwarded.
  const shared_ptr<Blob<float> > feature_blob = solver_[net_id_]->net()
        ->blob_by_name(string(blob_name));
  mxArray* mx_out = mxCreateCellMatrix(1, 1);
  mxArray* mx_blob = mxCreateNumericMatrix(feature_blob->count(), 
      1, mxSINGLE_CLASS, mxREAL);
  mxSetCell(mx_out, 0, mx_blob);
  float* data_ptr = reinterpret_cast<float*>(mxGetPr(mx_blob));
  switch (Caffe::mode()) {
  case Caffe::CPU:
    memcpy(data_ptr, feature_blob->cpu_data(),
        sizeof(float) * feature_blob->count());
    break;
  case Caffe::GPU:
    cudaMemcpy(data_ptr, feature_blob->gpu_data(),
        sizeof(float) * feature_blob->count(), cudaMemcpyDeviceToHost);
    break;
  default:
    LOG(FATAL) << "Unknown Caffe mode.";
  }  // switch (Caffe::mode())

  plhs[0] = mx_out;
}
/** -----------------------------------------------------------------
 ** Available commands.
 **/
struct handler_registry {
  string cmd;
  void (*func)(MEX_ARGS);
};

static handler_registry handlers[] = {
  // Public API functions
  { "forward",            forward         },
  { "update",             update          },
  { "init_finetune",      init_finetune   },
  { "extract_feature",    extract_feature },
  { "set_mode_cpu",       set_mode_cpu    },
  { "set_mode_gpu",       set_mode_gpu    },
  { "set_phase_train",    set_phase_train },
  { "set_phase_test",     set_phase_test  },
  { "set_device",         set_device      },
  { "set_batch_size",     set_batch_size  },
  { "set_net_id",         set_net_id      },
  // The end.
  { "END",                NULL            },
};


/** -----------------------------------------------------------------
 ** matlab entry point: caffe(api_command, arg1, arg2, ...)
 **/
void mexFunction(MEX_ARGS) {
  if (nrhs == 0) {
    LOG(ERROR) << "No API command given";
    mexErrMsgTxt("An API command is requires");
    return;
  }

  { // Handle input command
    char *cmd = mxArrayToString(prhs[0]);
    bool dispatched = false;
    // Dispatch to cmd handler
    for (int i = 0; handlers[i].func != NULL; i++) {
      if (handlers[i].cmd.compare(cmd) == 0) {
        handlers[i].func(nlhs, plhs, nrhs-1, prhs+1);
        dispatched = true;
        break;
      }
    }
    if (!dispatched) {
      LOG(ERROR) << "Unknown command `" << cmd << "'";
      mexErrMsgTxt("API command not recognized");
    }
    mxFree(cmd);
  }
}
