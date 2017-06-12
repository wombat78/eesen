// net/lstm-parallel-layer.h

// Copyright 2014  Jiayu DU (Jerry), Wei Li
// Copyright 2015  Yajie Miao

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

#ifndef EESEN_LSTM_PARALLEL_LAYER_H_
#define EESEN_LSTM_PARALLEL_LAYER_H_

#include "net/layer.h"
#include "net/trainable-layer.h"
#include "net/lstm-layer.h"
#include "net/utils-functions.h"
#include "gpucompute/cuda-math.h"

namespace kaldi {

class LstmParallel : public Lstm {
public:
    LstmParallel(int32 input_dim, int32 output_dim) : Lstm(input_dim, output_dim)
    { }
    ~LstmParallel()
    { }

    Layer* Copy() const { return new LstmParallel(*this); }
    LayerType GetType() const { return l_Lstm_Parallel; }
    LayerType GetTypeNonParal() const { return l_Lstm; }

    void SetSeqLengths(std::vector<int> &sequence_lengths) {
        sequence_lengths_ = sequence_lengths;
    }

    void PropagateFnc(const CuMatrixBase<BaseFloat> &in, CuMatrixBase<BaseFloat> *out) {
      int32 nstream_ = sequence_lengths_.size();  // the number of sequences to be processed in parallel
      KALDI_ASSERT(in.NumRows() % nstream_ == 0);
      int32 T = in.NumRows() / nstream_; 
      int32 S = nstream_;
        
      // initialize the propagation buffers
      propagate_buf_.Resize((T+2)*S, 7 * cell_dim_, kSetZero);

      CuSubMatrix<BaseFloat> YG(propagate_buf_.ColRange(0, cell_dim_));
      CuSubMatrix<BaseFloat> YI(propagate_buf_.ColRange(1 * cell_dim_, cell_dim_));
      CuSubMatrix<BaseFloat> YF(propagate_buf_.ColRange(2 * cell_dim_, cell_dim_));
      CuSubMatrix<BaseFloat> YO(propagate_buf_.ColRange(3 * cell_dim_, cell_dim_));
      CuSubMatrix<BaseFloat> YC(propagate_buf_.ColRange(4 * cell_dim_, cell_dim_));
      CuSubMatrix<BaseFloat> YH(propagate_buf_.ColRange(5 * cell_dim_, cell_dim_));
      CuSubMatrix<BaseFloat> YM(propagate_buf_.ColRange(6 * cell_dim_, cell_dim_));

      CuSubMatrix<BaseFloat> YGIFO(propagate_buf_.ColRange(0, 4 * cell_dim_));
      // no temporal recurrence involved in the inputs
      YGIFO.RowRange(1*S,T*S).AddMatMat(1.0, in, kNoTrans, wei_gifo_x_, kTrans, 0.0);
      YGIFO.RowRange(1*S,T*S).AddVecToRows(1.0, bias_);

      for (int t = 1; t <= T; t++) {
        // variables representing invidivual units/gates
        CuSubMatrix<BaseFloat> y_all(propagate_buf_.RowRange(t*S,S));
        CuSubMatrix<BaseFloat> y_g(YG.RowRange(t*S,S));  
        CuSubMatrix<BaseFloat> y_i(YI.RowRange(t*S,S));  
        CuSubMatrix<BaseFloat> y_f(YF.RowRange(t*S,S));  
        CuSubMatrix<BaseFloat> y_o(YO.RowRange(t*S,S));  
        CuSubMatrix<BaseFloat> y_c(YC.RowRange(t*S,S));  
        CuSubMatrix<BaseFloat> y_h(YH.RowRange(t*S,S));  
        CuSubMatrix<BaseFloat> y_m(YM.RowRange(t*S,S));  
        CuSubMatrix<BaseFloat> y_GIFO(YGIFO.RowRange(t*S,S));
            
        // add the recurrence of the previous memory cell to various gates/units 
        y_GIFO.AddMatMat(1.0, YM.RowRange((t-1)*S,S), kNoTrans, wei_gifo_m_, kTrans,  1.0);
        // input gate
        y_i.AddMatDiagVec(1.0, YC.RowRange((t-1)*S,S), kNoTrans, phole_i_c_, 1.0);
        // forget gate
        y_f.AddMatDiagVec(1.0, YC.RowRange((t-1)*S,S), kNoTrans, phole_f_c_, 1.0);
        // apply sigmoid/tanh functionis to squash the outputs
        y_i.Sigmoid(y_i);
        y_f.Sigmoid(y_f);
        y_g.Tanh(y_g);
    
        // memory cell
        y_c.AddMatDotMat(1.0, y_g, kNoTrans, y_i, kNoTrans, 0.0);
        y_c.AddMatDotMat(1.0, YC.RowRange((t-1)*S,S), kNoTrans, y_f, kNoTrans, 1.0);
 
        // the tanh-squashed version of c
        y_h.Tanh(y_c);
    
        // output gate
        y_o.AddMatDiagVec(1.0, y_c, kNoTrans, phole_o_c_, 1.0);
        y_o.Sigmoid(y_o);
    
        // the final output
        y_m.AddMatDotMat(1.0, y_h, kNoTrans, y_o, kNoTrans, 0.0);

//      for (int s = 0; s < S; s++) {
//        if (t > sequence_lengths_[s])
//            y_all.Row(s).SetZero();         
//      } 
      } // end of t
      
      out->CopyFromMat(YM.RowRange(S,T*S));
    }

    void BackpropagateFnc(const CuMatrixBase<BaseFloat> &in, const CuMatrixBase<BaseFloat> &out,
                            const CuMatrixBase<BaseFloat> &out_diff, CuMatrixBase<BaseFloat> *in_diff) {
      int32 nstream_ = sequence_lengths_.size();  // the number of sequences to be processed in parallel
      KALDI_ASSERT(in.NumRows() % nstream_ == 0);
      int32 T = in.NumRows() / nstream_;
      int32 S = nstream_;
 
      // initialize the back-propagation buffer
      backpropagate_buf_.Resize((T+2)*S, 7 * cell_dim_, kSetZero);

      // get the activations of the gates/units from the feedforward buffer; these variabiles will be used
      // in gradients computation
      CuSubMatrix<BaseFloat> YG(propagate_buf_.ColRange(0, cell_dim_));
      CuSubMatrix<BaseFloat> YI(propagate_buf_.ColRange(1 * cell_dim_, cell_dim_));
      CuSubMatrix<BaseFloat> YF(propagate_buf_.ColRange(2 * cell_dim_, cell_dim_));
      CuSubMatrix<BaseFloat> YO(propagate_buf_.ColRange(3 * cell_dim_, cell_dim_));
      CuSubMatrix<BaseFloat> YC(propagate_buf_.ColRange(4 * cell_dim_, cell_dim_));
      CuSubMatrix<BaseFloat> YH(propagate_buf_.ColRange(5 * cell_dim_, cell_dim_));
      CuSubMatrix<BaseFloat> YM(propagate_buf_.ColRange(6 * cell_dim_, cell_dim_));

      // errors back-propagated to individual gates/units
      CuSubMatrix<BaseFloat> DG(backpropagate_buf_.ColRange(0, cell_dim_));
      CuSubMatrix<BaseFloat> DI(backpropagate_buf_.ColRange(1 * cell_dim_, cell_dim_));
      CuSubMatrix<BaseFloat> DF(backpropagate_buf_.ColRange(2 * cell_dim_, cell_dim_));
      CuSubMatrix<BaseFloat> DO(backpropagate_buf_.ColRange(3 * cell_dim_, cell_dim_));
      CuSubMatrix<BaseFloat> DC(backpropagate_buf_.ColRange(4 * cell_dim_, cell_dim_));
      CuSubMatrix<BaseFloat> DH(backpropagate_buf_.ColRange(5 * cell_dim_, cell_dim_));
      CuSubMatrix<BaseFloat> DM(backpropagate_buf_.ColRange(6 * cell_dim_, cell_dim_));
      CuSubMatrix<BaseFloat> DGIFO(backpropagate_buf_.ColRange(0, 4 * cell_dim_));

      //  assume that the fist half of out_diff is about the forward layer
      DM.RowRange(1*S,T*S).CopyFromMat(out_diff);

      for (int t = T; t >= 1; t--) {
        // variables representing activations of invidivual units/gates
        CuSubMatrix<BaseFloat> y_g(YG.RowRange(t*S, S));  
        CuSubMatrix<BaseFloat> y_i(YI.RowRange(t*S, S));  
        CuSubMatrix<BaseFloat> y_f(YF.RowRange(t*S, S));  
        CuSubMatrix<BaseFloat> y_o(YO.RowRange(t*S, S));  
        CuSubMatrix<BaseFloat> y_c(YC.RowRange(t*S, S));  
        CuSubMatrix<BaseFloat> y_h(YH.RowRange(t*S, S));  
        CuSubMatrix<BaseFloat> y_m(YM.RowRange(t*S, S));  
        // variables representing errors of invidivual units/gates
        CuSubMatrix<BaseFloat> d_g(DG.RowRange(t*S, S));
        CuSubMatrix<BaseFloat> d_i(DI.RowRange(t*S, S));
        CuSubMatrix<BaseFloat> d_f(DF.RowRange(t*S, S));
        CuSubMatrix<BaseFloat> d_o(DO.RowRange(t*S, S));
        CuSubMatrix<BaseFloat> d_c(DC.RowRange(t*S, S));
        CuSubMatrix<BaseFloat> d_h(DH.RowRange(t*S, S));
        CuSubMatrix<BaseFloat> d_m(DM.RowRange(t*S, S));
        CuSubMatrix<BaseFloat> d_all(backpropagate_buf_.RowRange(t*S, S));   
 
        // d_m comes from two parts: errors from the upper layer and errors from the following frame (t+1)
        d_m.AddMatMat(1.0, DGIFO.RowRange((t+1)*S,S), kNoTrans, wei_gifo_m_, kNoTrans, 1.0);
    
        // d_h
        d_h.AddMatDotMat(1.0, d_m, kNoTrans, y_o, kNoTrans, 0.0);
        d_h.DiffTanh(y_h, d_h);
    
        // d_o
        d_o.AddMatDotMat(1.0, d_m, kNoTrans, y_h, kNoTrans, 0.0);
        d_o.DiffSigmoid(y_o, d_o);
    
        // d_c
        d_c.AddMat(1.0, d_h);  
        d_c.AddMatDotMat(1.0, DC.RowRange((t+1)*S,S), kNoTrans, YF.RowRange((t+1)*S,S), kNoTrans, 1.0);
        d_c.AddMatDiagVec(1.0, DI.RowRange((t+1)*S,S), kNoTrans, phole_i_c_, 1.0);
        d_c.AddMatDiagVec(1.0, DF.RowRange((t+1)*S,S), kNoTrans, phole_f_c_, 1.0);
        d_c.AddMatDiagVec(1.0, d_o, kNoTrans, phole_o_c_, 1.0);
    
        // d_f
        d_f.AddMatDotMat(1.0, d_c, kNoTrans, YC.RowRange((t-1)*S,S), kNoTrans, 0.0);
        d_f.DiffSigmoid(y_f, d_f);
    
        // d_i
        d_i.AddMatDotMat(1.0, d_c, kNoTrans, y_g, kNoTrans, 0.0);
        d_i.DiffSigmoid(y_i, d_i);
    
        // d_g
        d_g.AddMatDotMat(1.0, d_c, kNoTrans, y_i, kNoTrans, 0.0);
        d_g.DiffTanh(y_g, d_g);

//      for (int s = 0; s < S; s++) {
//        if (t > sequence_lengths_[s])
//          d_all.Row(s).SetZero();            
//      }
      }  // end of t

      // errors back-propagated to the inputs
      in_diff->AddMatMat(1.0, DGIFO.RowRange(1*S,T*S), kNoTrans, wei_gifo_x_, kNoTrans, 0.0);
      //  updates to the model parameters
      const BaseFloat mmt = opts_.momentum;
      wei_gifo_x_corr_.AddMatMat(1.0, DGIFO.RowRange(1*S, T*S), kTrans, in, kNoTrans, mmt);
      wei_gifo_m_corr_.AddMatMat(1.0, DGIFO.RowRange(1*S, T*S), kTrans, YM.RowRange(0*S,T*S), kNoTrans, mmt);
      bias_corr_.AddRowSumMat(1.0, DGIFO.RowRange(1*S, T*S), mmt);
      phole_i_c_corr_.AddDiagMatMat(1.0, DI.RowRange(1*S, T*S), kTrans, YC.RowRange(0*S, T*S), kNoTrans, mmt);
      phole_f_c_corr_.AddDiagMatMat(1.0, DF.RowRange(1*S, T*S), kTrans, YC.RowRange(0*S, T*S), kNoTrans, mmt);
      phole_o_c_corr_.AddDiagMatMat(1.0, DO.RowRange(1*S, T*S), kTrans, YC.RowRange(1*S, T*S), kNoTrans, mmt);
    }

private:

    int32 nstream_;
    std::vector<int> sequence_lengths_;

};
} // namespace kaldi

#endif
