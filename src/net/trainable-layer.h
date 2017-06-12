// net/trainable-layer.h

// Copyright 2011-2013  Brno University of Technology (Author: Karel Vesely)
//                2015  Yajie Miao, Hang Su

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.



#ifndef EESEN_TRAINABLE_LAYER_H_
#define EESEN_TRAINABLE_LAYER_H_


#include "base/kaldi-common.h"
#include "cpucompute/matrix-lib.h"
#include "gpucompute/cuda-matrix.h"
#include "gpucompute/cuda-vector.h"
#include "net/train-opts.h"
#include "net/layer.h"

#include <iostream>

namespace kaldi {

/**
 * Class TrainableLayer is a Layer which has trainable parameters,
 * contains SGD training hyper-parameters in NetTrainOptions.
 */
class TrainableLayer : public Layer {
 public: 
  TrainableLayer(int32 input_dim, int32 output_dim)
    : Layer(input_dim, output_dim) { }
  virtual ~TrainableLayer() { }

  /// Check if contains trainable parameters 
  bool IsTrainable() const { 
    return true; 
  }

  /// Number of trainable parameters
  virtual int32 NumParams() const = 0;
  virtual void GetParams(Vector<BaseFloat> *params) const = 0;

  /// Compute gradient and update parameters
  virtual void Update(const CuMatrixBase<BaseFloat> &input,
                      const CuMatrixBase<BaseFloat> &diff) = 0;
  
  virtual void Scale(BaseFloat scale) = 0;

  virtual void Add(BaseFloat scale, const TrainableLayer & layer_other) = 0;

  /// Sets the training options to the component
  virtual void SetTrainOptions(const NetTrainOptions &opts) {
    opts_ = opts;
  }
  /// Gets the training options from the component
  const NetTrainOptions& GetTrainOptions() const { 
    return opts_; 
  }

  virtual void InitData(std::istream &is) = 0;

 protected:
  /// Option-class with training hyper-parameters
  NetTrainOptions opts_; 
};

} // namespace kaldi


#endif
