/* Copyright 2016 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow_serving/batching/batching_session.h"

#include <stddef.h>

#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/tensor_util.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/notification.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/strings/str_util.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow_serving/servables/tensorflow/serving_session.h"
#include "tensorflow_serving/util/cleanup.h"
#include "tensorflow_serving/util/hash.h"

namespace tensorflow {
namespace serving {

namespace {

string TensorSignatureDebugString(const TensorSignature& signature) {
  return strings::StrCat("{input_tensors: <",
                         str_util::Join(signature.input_tensors, ", "),
                         ">, output_tensors: <",
                         str_util::Join(signature.output_tensors, ", "), ">}");
}

struct HashTensorSignature {
  uint64 operator()(const TensorSignature& signature) const {
    uint64 hash = 0xDECAFCAFFE /* seed */;
    for (const string& input_tensor : signature.input_tensors) {
      hash = HashCombine(hash, std::hash<string>()(input_tensor));
    }
    for (const string& output_tensor : signature.output_tensors) {
      hash = HashCombine(hash, std::hash<string>()(output_tensor));
    }
    return hash;
  }
};

struct EqTensorSignature {
  bool operator()(const TensorSignature& lhs,
                  const TensorSignature& rhs) const {
    return lhs.input_tensors == rhs.input_tensors &&
           lhs.output_tensors == rhs.output_tensors;
  }
};

// Constructs a TensorSignature from a Run() call's 'inputs' and
// 'output_tensor_names' arguments.
TensorSignature TensorSignatureFromRunArgs(
    const std::vector<std::pair<string, Tensor>>& inputs,
    const std::vector<string>& output_tensor_names) {
  TensorSignature signature;
  for (const auto& entry : inputs) {
    const string& tensor_name = entry.first;
    signature.input_tensors.insert(tensor_name);
  }
  for (const string& output_tensor_name : output_tensor_names) {
    signature.output_tensors.insert(output_tensor_name);
  }
  return signature;
}

}  // namespace

TensorSignature TensorSignatureFromSignatureDef(
    const SignatureDef& signature_def) {
  return TensorSignatureFromSignatureDefs({signature_def});
}

TensorSignature TensorSignatureFromSignatureDefs(
    const std::vector<SignatureDef>& signature_defs) {
  TensorSignature tensor_signature;
  for (const SignatureDef& signature_def : signature_defs) {
    for (const auto& entry : signature_def.inputs()) {
      const TensorInfo& tensor_info = entry.second;
      tensor_signature.input_tensors.insert(tensor_info.name());
    }
    for (const auto& entry : signature_def.outputs()) {
      const TensorInfo& tensor_info = entry.second;
      tensor_signature.output_tensors.insert(tensor_info.name());
    }
  }
  return tensor_signature;
}

// A session that performs batching on top of a wrapped session. See the
// documentation in batching_session.h for details and constraints.
class BatchingSession : public ServingSession {
 public:
  // Constructs a BatchingSession. Arguments:
  // - 'options' contains parameters. See batching_session.h.
  // - 'wrapped' is the session to wrap with batching.
  // - 'signatures_with_scheduler_creators' specifies the set of supported
  //   signatures, and for each one supplies a lambda to construct a batch
  //   scheduler given a process-batch callback. See batching_session.h for
  //   example usage.
  static Status Create(
      const BatchingSessionOptions& options, std::unique_ptr<Session> wrapped,
      const std::vector<SignatureWithBatchingSessionSchedulerCreator>&
          signatures_with_scheduler_creators,
      std::unique_ptr<BatchingSession>* result);

  ~BatchingSession() override = default;

  Status Run(const std::vector<std::pair<string, Tensor>>& inputs,
             const std::vector<string>& output_tensor_names,
             const std::vector<string>& target_node_names,
             std::vector<Tensor>* outputs) override;

 private:
  explicit BatchingSession(const BatchingSessionOptions& options);

  // Computes the size of an input tensor list for batching purposes, by
  // analyzing the 0th dimension size of each of the tensors. All tensors in the
  // list must have the same 0th dimension size to be batchable. If the sizes
  // are not all identical, returns an error.
  Status ComputeInputSize(const std::vector<std::pair<string, Tensor>>& inputs,
                          size_t* size) const;

  // Returns the smallest entry in 'options_.allowed_batch_sizes' that is
  // greater than or equal to 'batch_size'. If 'options_.allowed_batch_sizes' is
  // empty, simply returns 'batch_size'.
  int RoundToLowestAllowedBatchSize(int batch_size) const;

  // Merges the input tensors in a batch, via concatenation of correspondingly-
  // named tensors. Puts the merged inputs in the order they are in in the
  // signature. Assumes 'batch' is non-empty. Returns an error if there are any
  // mismatches among the tasks in the batch that violate the constraints for
  // batchability.
  Status MergeInputTensors(
      const TensorSignature& signature, const Batch<BatchingSessionTask>& batch,
      std::vector<std::pair<string, Tensor>>* merged_inputs);

  // Splits the output of a batched call to 'wrapped_->Run()' into individual
  // task outputs. Assumes the output tensor order matches the signature.
  Status SplitOutputTensors(const TensorSignature& signature,
                            const std::vector<Tensor>& combined_outputs,
                            Batch<BatchingSessionTask>* batch);

  // Processes one batch of Run() calls with 'signature'. Called by
  // 'batch_scheduler_' in a batch thread.
  void ProcessBatch(const TensorSignature& signature,
                    std::unique_ptr<Batch<BatchingSessionTask>> batch);

  const BatchingSessionOptions options_;

  std::unique_ptr<Session> wrapped_;
  std::unordered_map<TensorSignature,
                     std::unique_ptr<BatchScheduler<BatchingSessionTask>>,
                     HashTensorSignature, EqTensorSignature>
      batch_schedulers_;

  TF_DISALLOW_COPY_AND_ASSIGN(BatchingSession);
};

Status BatchingSession::Create(
    const BatchingSessionOptions& options, std::unique_ptr<Session> wrapped,
    const std::vector<SignatureWithBatchingSessionSchedulerCreator>&
        signatures_with_scheduler_creators,
    std::unique_ptr<BatchingSession>* result) {
  auto batching_session =
      std::unique_ptr<BatchingSession>(new BatchingSession(options));
  BatchingSession* raw_batching_session = batching_session.get();
  batching_session->wrapped_ = std::move(wrapped);

  for (const auto& entry : signatures_with_scheduler_creators) {
    const TensorSignature& signature = entry.signature;
    const BatchingSessionSchedulerCreator& scheduler_creator =
        entry.scheduler_creator;

    std::unique_ptr<BatchScheduler<BatchingSessionTask>> batch_scheduler;
    TF_RETURN_IF_ERROR(scheduler_creator(
        [signature, raw_batching_session](
            std::unique_ptr<Batch<BatchingSessionTask>> batch) {
          raw_batching_session->ProcessBatch(signature, std::move(batch));
        },
        &batch_scheduler));
    batching_session->batch_schedulers_[signature] = std::move(batch_scheduler);
  }

  *result = std::move(batching_session);
  return Status::OK();
}

Status BatchingSession::Run(
    const std::vector<std::pair<string, Tensor>>& inputs,
    const std::vector<string>& output_tensor_names,
    const std::vector<string>& target_node_names,
    std::vector<Tensor>* outputs) {
  if (!target_node_names.empty()) {
    return errors::PermissionDenied(
        "BatchingSession does not support target nodes");
  }

  const TensorSignature signature =
      TensorSignatureFromRunArgs(inputs, output_tensor_names);
  auto batch_scheduler_it = batch_schedulers_.find(signature);
  if (batch_scheduler_it == batch_schedulers_.end()) {
    // We have a Run() call that doesn't match one of our batching signatures.
    // Run it in-line.
    LOG(WARNING) << "Request doesn't match any declared signature. Bypassing "
                    "batcher. Request signature is: "
                 << TensorSignatureDebugString(signature);
    return wrapped_->Run(inputs, output_tensor_names, target_node_names,
                         outputs);
  }
  BatchScheduler<BatchingSessionTask>* batch_scheduler =
      batch_scheduler_it->second.get();

  outputs->clear();

  Notification done;
  Status status;
  auto task = std::unique_ptr<BatchingSessionTask>(new BatchingSessionTask);
  TF_RETURN_IF_ERROR(ComputeInputSize(inputs, &task->zeroth_dim_size));
  task->inputs = &inputs;
  task->output_tensor_names = &output_tensor_names;
  task->done = &done;
  task->status = &status;
  task->outputs = outputs;

  TF_RETURN_IF_ERROR(batch_scheduler->Schedule(&task));
  done.WaitForNotification();
  return status;
}

BatchingSession::BatchingSession(const BatchingSessionOptions& options)
    : options_(options) {}

Status BatchingSession::ComputeInputSize(
    const std::vector<std::pair<string, Tensor>>& inputs, size_t* size) const {
  if (inputs.size() == 0) {
    return errors::InvalidArgument(
        "Batching session Run() must have at least one input tensor");
  }

  bool first = true;
  for (const auto& entry : inputs) {
    const Tensor& tensor = entry.second;

    if (tensor.shape().dims() == 0) {
      return errors::InvalidArgument(
          "Batching session Run() input tensors must have at least one "
          "dimension");
    }
    const size_t this_size = tensor.shape().dim_size(0);

    if (first) {
      *size = this_size;
      first = false;
    } else {
      if (this_size != *size) {
        return errors::InvalidArgument(
            "Batching session Run() input tensors must have equal "
            "0th-dimension size");
      }
    }
  }
  return Status::OK();
}

int BatchingSession::RoundToLowestAllowedBatchSize(int batch_size) const {
  if (options_.allowed_batch_sizes.empty()) {
    return batch_size;
  }
  for (int allowed_size : options_.allowed_batch_sizes) {
    if (allowed_size >= batch_size) {
      return allowed_size;
    }
  }
  LOG(ERROR) << "Maximum batch size greater than largest allowed size; "
                "ignoring allowed sizes constraint";
  return batch_size;
}

Status BatchingSession::MergeInputTensors(
    const TensorSignature& signature, const Batch<BatchingSessionTask>& batch,
    std::vector<std::pair<string, Tensor>>* merged_inputs) {
  DCHECK_GE(batch.num_tasks(), 1);
  if (batch.num_tasks() < 1) {
    return errors::Internal("Batch size expected to be positive; was ",
                            batch.num_tasks());
  }

  const int padding_size =
      RoundToLowestAllowedBatchSize(batch.size()) - batch.size();

  // For each input tensor name, a vector of tensors from the individual tasks.
  std::map<string, std::vector<Tensor>> tensors_to_merge;

  // Populate 'tensors_to_merge'.
  for (int i = 0; i < batch.num_tasks(); ++i) {
    const std::vector<std::pair<string, Tensor>>& task_inputs =
        *batch.task(i).inputs;
    for (const auto& entry : task_inputs) {
      const string& tensor_name = entry.first;
      const Tensor& tensor = entry.second;

      std::vector<Tensor>& tensor_vec = tensors_to_merge[tensor_name];
      tensor_vec.push_back(tensor);

      if (i == batch.num_tasks() - 1 && padding_size > 0) {
        // This is the last task. Insert padding.
        //
        // Use the first row of this task's tensor as the padding data. (We know
        // it represents a valid input tensor row, so it should always be safe
        // to use for padding.)
        //
        // Slice() operates on the 0th dimension, which is the batch dimension.
        // It avoids a deep copy, which is a nice efficiency bonus.
        const Tensor padding_tensor = tensor.Slice(0, 1);
        for (int i = 0; i < padding_size; ++i) {
          tensor_vec.push_back(padding_tensor);
        }
      }
    }
  }

  // Merge the tensors.
  DCHECK_EQ(signature.input_tensors.size(), tensors_to_merge.size());
  if (tensors_to_merge.size() != signature.input_tensors.size()) {
    return errors::Internal(
        "One or more tasks does not conform to batch signature");
  }
  for (const string& tensor_name : signature.input_tensors) {
    auto tensors = tensors_to_merge.find(tensor_name);
    DCHECK(tensors != tensors_to_merge.end());
    if (tensors == tensors_to_merge.end()) {
      return errors::Internal(
          "One or more tasks does not conform to batch signature");
    }
    merged_inputs->push_back({tensor_name, tensor::Concat(tensors->second)});
  }

  return Status::OK();
}

Status BatchingSession::SplitOutputTensors(
    const TensorSignature& signature,
    const std::vector<Tensor>& combined_outputs,
    Batch<BatchingSessionTask>* batch) {
  DCHECK_GE(batch->num_tasks(), 1);
  if (batch->num_tasks() < 1) {
    return errors::Internal("Batch size expected to be positive; was ",
                            batch->num_tasks());
  }

  std::vector<int64> task_sizes_plus_optional_padding;
  for (int i = 0; i < batch->num_tasks(); ++i) {
    task_sizes_plus_optional_padding.push_back(batch->task(i).zeroth_dim_size);
  }
  const int padding_size =
      RoundToLowestAllowedBatchSize(batch->size()) - batch->size();
  if (padding_size > 0) {
    task_sizes_plus_optional_padding.push_back(padding_size);
  }

  // For each output tensor name, a divided-up tensor with one entry per task.
  std::map<string, std::vector<Tensor>> split_tensors;

  // Populate 'split_tensors'.
  DCHECK_EQ(signature.output_tensors.size(), combined_outputs.size());
  if (combined_outputs.size() != signature.output_tensors.size()) {
    return errors::Internal("Wrong number of batched output tensors");
  }
  const std::vector<string> output_tensors(signature.output_tensors.begin(),
                                           signature.output_tensors.end());
  for (int i = 0; i < output_tensors.size(); ++i) {
    const string& tensor_name = output_tensors[i];
    const Tensor& tensor = combined_outputs[i];

    if (tensor.shape().dims() == 0) {
      return errors::FailedPrecondition(
          "Batched output tensor has 0 dimensions");
    }
    if (tensor.shape().dim_size(0) != batch->size() + padding_size) {
      return errors::FailedPrecondition(
          "Batched output tensor's 0th dimension does not equal the sum of the "
          "0th dimension sizes of the input tensors");
    }

    std::vector<Tensor> split_tensor =
        tensor::Split(tensor, task_sizes_plus_optional_padding);
    DCHECK_EQ(split_tensor.size(), task_sizes_plus_optional_padding.size());
    if (split_tensor.size() != task_sizes_plus_optional_padding.size()) {
      return errors::Internal(
          "Tensor split operation did not work as expected; got ",
          split_tensor.size(), " splits; expected ",
          task_sizes_plus_optional_padding.size());
    }
    split_tensors[tensor_name] = std::move(split_tensor);
  }

  for (int i = 0; i < batch->num_tasks(); ++i) {
    BatchingSessionTask* task = batch->mutable_task(i);
    for (const string& tensor_name : *task->output_tensor_names) {
      auto split_tensor = split_tensors.find(tensor_name);
      DCHECK(split_tensor != split_tensors.end());
      if (split_tensor == split_tensors.end()) {
        return errors::Internal("Task does not conform to batch signature");
      }
      task->outputs->push_back(split_tensor->second[i]);
    }
  }
  // (Ignore a possible final split_tensors entry containing the padding.)

  return Status::OK();
}

void BatchingSession::ProcessBatch(
    const TensorSignature& signature,
    std::unique_ptr<Batch<BatchingSessionTask>> batch) {
  // As a possible performance optimization, consider overlapping the tensor
  // concatenation with waiting for the batch to close (i.e. do the
  // concatenation incrementally as tasks stream into the batch).
  batch->WaitUntilClosed();

  if (batch->empty()) {
    return;
  }

  Status status;

  // Regardless of the outcome, we need to propagate the status to the
  // individual tasks and signal that they are done. We use MakeCleanup() to
  // ensure that this happens no matter how we exit the method below.
  auto finally = MakeCleanup([&status, &batch] {
    for (int i = 0; i < batch->num_tasks(); ++i) {
      *batch->mutable_task(i)->status = status;
      batch->mutable_task(i)->done->Notify();
    }
  });

  std::vector<std::pair<string, Tensor>> merged_inputs;
  status = MergeInputTensors(signature, *batch, &merged_inputs);
  if (!status.ok()) {
    return;
  }

  const std::vector<string> output_tensor_names(
      signature.output_tensors.begin(), signature.output_tensors.end());
  std::vector<Tensor> combined_outputs;
  status = wrapped_->Run(merged_inputs, output_tensor_names,
                         {} /* target node names */, &combined_outputs);
  if (!status.ok()) {
    return;
  }

  status = SplitOutputTensors(signature, combined_outputs, batch.get());
}

Status CreateBatchingSession(
    const BatchingSessionOptions& options,
    const std::vector<SignatureWithBatchingSessionSchedulerCreator>&
        signatures_with_scheduler_creators,
    std::unique_ptr<Session> session,
    std::unique_ptr<Session>* batching_session) {
  std::unique_ptr<BatchingSession> internal_batching_session;
  TF_RETURN_IF_ERROR(BatchingSession::Create(options, std::move(session),
                                             signatures_with_scheduler_creators,
                                             &internal_batching_session));
  *batching_session = std::move(internal_batching_session);
  return Status::OK();
}

Status CreateBasicBatchingSession(
    const BasicBatchScheduler<BatchingSessionTask>::Options& schedule_options,
    const BatchingSessionOptions& batching_session_options,
    const TensorSignature& signature, std::unique_ptr<Session> session,
    std::unique_ptr<Session>* batching_session) {
  if (!batching_session_options.allowed_batch_sizes.empty()) {
    if (batching_session_options.allowed_batch_sizes.back() !=
        schedule_options.max_batch_size) {
      return errors::InvalidArgument(
          "Last entry in allowed_batch_sizes must match max_batch_size; last "
          "entry was ",
          batching_session_options.allowed_batch_sizes.back(), "; expected ",
          schedule_options.max_batch_size);
    }
  }

  auto scheduler_creator = [schedule_options](
      std::function<void(std::unique_ptr<Batch<BatchingSessionTask>>)>
          process_batch_callback,
      std::unique_ptr<BatchScheduler<BatchingSessionTask>>* batch_scheduler) {
    std::unique_ptr<BasicBatchScheduler<BatchingSessionTask>>
        basic_batch_scheduler;
    TF_RETURN_IF_ERROR(BasicBatchScheduler<BatchingSessionTask>::Create(
        schedule_options, process_batch_callback, &basic_batch_scheduler));
    *batch_scheduler = std::move(basic_batch_scheduler);
    return Status::OK();
  };
  return CreateBatchingSession(batching_session_options,
                               {{signature, scheduler_creator}},
                               std::move(session), batching_session);
}

}  // namespace serving
}  // namespace tensorflow
