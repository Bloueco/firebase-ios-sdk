/*
 * Copyright 2018 Google
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_REMOTE_GRPC_STREAM_H_
#define FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_REMOTE_GRPC_STREAM_H_

#include <memory>
#include <map>
#include <queue>
#include <string>
#include <vector>
#include <utility>

#include "Firestore/core/src/firebase/firestore/remote/grpc_stream_observer.h"
#include "Firestore/core/src/firebase/firestore/remote/stream_operation.h"
#include "Firestore/core/src/firebase/firestore/util/async_queue.h"
#include "Firestore/core/src/firebase/firestore/util/status.h"
#include "grpcpp/client_context.h"
#include "grpcpp/generic/generic_stub.h"
#include "grpcpp/support/byte_buffer.h"

namespace firebase {
namespace firestore {
namespace remote {

class GrpcStream;

namespace internal {

/**
 * `BufferedWriter` accepts serialized protos ("writes") on its queue and
 * writes them to the gRPC stream one by one. Only one write
 * may be in progress ("active") at any given time.
 *
 * Writes are put on the queue using `EnqueueWrite`; if no other write is
 * currently in progress, a write will be issued with the given proto
 * immediately, otherwise, the proto will be "buffered" (put on the queue in
 * this `BufferedWriter`). When a write becomes active, a `StreamWrite`
 * operation is created with the proto and immediately executed; a write is
 * active from the moment it is executed and until `DequeueNextWrite` is called
 * on the `BufferedWriter`. `DequeueNextWrite` makes the next write active, if
 * any.
 *
 * `BufferedWriter` does not store any of the operations it creates.
 *
 * This class exists to help Firestore streams adhere to the gRPC requirement
 * that only one write operation may be active at any given time.
 */
class BufferedWriter {
 public:
  explicit BufferedWriter(GrpcStream* stream) : stream_{stream} {
  }

  // Returns the newly-created write operation if the given `write` became
  // active, null pointer otherwise.
  StreamWrite* EnqueueWrite(grpc::ByteBuffer&& write);
  // Returns the newly-created write operation if there was a next write in the
  // queue, or nullptr if the queue was empty.
  StreamWrite* DequeueNextWrite();

 private:
  StreamWrite* TryStartWrite();

  // Needed to create new `StreamWrite`s.
  GrpcStream* stream_ = nullptr;

  std::queue<grpc::ByteBuffer> queue_;
  bool has_active_write_ = false;
};

}  // namespace internal

/**
 * A gRPC bidirectional stream that notifies the given `observer` about stream
 * events.
 *
 * The stream has to be explicitly opened (via `Start`) before it can be used.
 * The stream is always listening for new messages from the server. The stream
 * can be used to send messages to the server (via `Write`); messages are queued
 * and sent out one by one. Both sent and received messages are raw bytes;
 * serialization and deserialization are left to the caller.
 *
 * The observer will be notified about the following events:
 * - stream has been started;
 * - stream has received a new message from the server;
 * - stream has been interrupted with an error. All errors are unrecoverable.
 *
 * Note that the stream will _not_ notify the observer about finish if the
 * finish was initiated by the client.
 *
 * The stream is disposable; once it finishes, it cannot be restarted.
 *
 * This class is essentially a wrapper over
 * `grpc::GenericClientAsyncReaderWriter`. See the source file for comments on
 * implementation details.
 */
class GrpcStream {
 public:
  using MetadataT = std::multimap<grpc::string_ref, grpc::string_ref>;

  GrpcStream(std::unique_ptr<grpc::ClientContext> context,
             std::unique_ptr<grpc::GenericClientAsyncReaderWriter> call,
             GrpcStreamObserver* observer,
             util::AsyncQueue* firestore_queue);
  ~GrpcStream();

  void Start();

  // Can only be called once the stream has opened.
  void Write(grpc::ByteBuffer&& message);

  // Does not produce a notification. Once this method is called, the stream can
  // no longer be used.
  //
  // This is a blocking operation; blocking time is expected to be in the order
  // of tens of milliseconds.

  // Can be called on a stream before it opens.
  void Finish();

  /**
   * Writes the given message and finishes the stream as soon as the write
   * succeeds. The final write is done on a best-effort basis; the return value
   * indicates whether the final write went through.
   *
   * This is a blocking operation; blocking time is expected to be in the order
   * of tens of milliseconds.
   *
   * Can only be called once the stream has opened.
   */
  bool WriteAndFinish(grpc::ByteBuffer&& message);

  bool IsFinished() const {
    return observer_ == nullptr;
  }

  /**
   * Returns the metadata received from the server.
   *
   * Can only be called once the stream has opened.
   */
  MetadataT GetResponseHeaders() const;

  // These are callbacks from the various `StreamOperation` classes that
  // shouldn't otherwise be called.
  void OnStart();
  void OnRead(const grpc::ByteBuffer& message);
  void OnWrite();
  void OnOperationFailed();
  void OnFinishedByServer(const grpc::Status& status);
  void OnFinishedByClient();
  void RemoveOperation(const StreamOperation* to_remove);
  grpc::GenericClientAsyncReaderWriter* call() {
    return call_.get();
  }
  util::AsyncQueue* firestore_queue() {
    return firestore_queue_;
  }

 private:
  void Read();
  StreamWrite* BufferedWrite(grpc::ByteBuffer&& message);

  void UnsetObserver() {
    observer_ = nullptr;
  }

  // A blocking function that waits until all the operations issued by this
  // stream come out from the gRPC completion queue. Once they do, it is safe to
  // delete this `GrpcStream` (thus releasing `grpc::ClientContext`). This
  // function should only be called during the stream finish.
  //
  // Important: before calling this function, the caller must be sure that any
  // pending operations on the gRPC completion queue will come back quickly
  // (either because the call has failed, or because the call has been
  // canceled). Otherwise, this function will block indefinitely.
  void FastFinishOperationsBlocking();

  void Execute(StreamOperation* operation) {
    operation->Execute();
    operations_.push_back(operation);
  }

  // The gRPC objects that have to be valid until the last gRPC operation
  // associated with this call finishes. Note that `grpc::ClientContext` is
  // _not_ reference-counted.
  //
  // Important: `call_` has to be destroyed before `context_`, so declaration
  // order matters here. Despite the unique pointer, `call_` is actually
  // a non-owning handle, and the memory it refers to (part of a gRPC memory
  // arena) will be released once `context_` (which is owning) is released.
  std::unique_ptr<grpc::ClientContext> context_;
  std::unique_ptr<grpc::GenericClientAsyncReaderWriter> call_;

  util::AsyncQueue* firestore_queue_ = nullptr;

  GrpcStreamObserver* observer_ = nullptr;
  internal::BufferedWriter buffered_writer_;

  std::vector<StreamOperation*> operations_;

  bool is_finishing_ = false;
};

}  // namespace remote
}  // namespace firestore
}  // namespace firebase

#endif  // FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_REMOTE_GRPC_STREAM_H_