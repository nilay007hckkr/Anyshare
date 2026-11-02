// JNI bridge: com.example.myapplication.transfer.NativeBridge <-> ASP/1 core.
//
// Threading model: every native method is blocking and is called from a Java
// worker thread. All callbacks into Java happen synchronously on that same
// thread, so no AttachCurrentThread / global references are needed. The Java
// side marshals UI updates to the main thread.
//
// Java owns the transport (BluetoothSocket or java.net.Socket); C++ sees it as
// an IByteStream over its InputStream/OutputStream. Cancelling = Java closes
// the socket and/or returns true from Callbacks.isCancelled().
#include <jni.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "ConnectionInfoParser.hpp"
#include "Protocol.hpp"
#include "TransferSession.hpp"

using namespace anyshare;

namespace {

constexpr const char* kResultClass = "com/example/myapplication/transfer/NativeBridge$Result";
constexpr jint kChunk = 256 * 1024;

// --- Strings: real UTF-8 <-> UTF-16 (JNI "modified UTF-8" breaks emoji) -------

std::string toUtf8(JNIEnv* env, jstring s) {
    if (!s) return {};
    const jsize len = env->GetStringLength(s);
    const jchar* chars = env->GetStringChars(s, nullptr);
    std::string out;
    out.reserve(static_cast<std::size_t>(len));
    for (jsize i = 0; i < len; ++i) {
        uint32_t c = chars[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < len && chars[i + 1] >= 0xDC00 && chars[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (chars[++i] - 0xDC00);
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;   // lone surrogate
        }
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    env->ReleaseStringChars(s, chars);
    return out;
}

jstring toJava(JNIEnv* env, const std::string& s) {
    std::vector<jchar> out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        auto b = static_cast<unsigned char>(s[i]);
        uint32_t c;
        std::size_t len = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 0;
        bool valid = len != 0 && i + len <= s.size();
        for (std::size_t k = 1; valid && k < len; ++k) valid = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
        if (!valid) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        c = len == 1 ? b : len == 2 ? (b & 0x1F) : len == 3 ? (b & 0x0F) : (b & 0x07);
        for (std::size_t k = 1; k < len; ++k) c = (c << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(static_cast<jchar>(0xD800 + (c >> 10)));
            out.push_back(static_cast<jchar>(0xDC00 + (c & 0x3FF)));
        } else {
            out.push_back(static_cast<jchar>(c));
        }
        i += len;
    }
    return env->NewString(out.data(), static_cast<jsize>(out.size()));
}

// Takes and clears the pending Java exception, returning its description.
std::string takeException(JNIEnv* env) {
    jthrowable ex = env->ExceptionOccurred();
    if (!ex) return {};
    env->ExceptionClear();
    jclass objectClass = env->FindClass("java/lang/Object");
    jmethodID toString = env->GetMethodID(objectClass, "toString", "()Ljava/lang/String;");
    auto text = static_cast<jstring>(env->CallObjectMethod(ex, toString));
    std::string message = env->ExceptionCheck() ? (env->ExceptionClear(), "java exception") : toUtf8(env, text);
    env->DeleteLocalRef(text);
    env->DeleteLocalRef(objectClass);
    env->DeleteLocalRef(ex);
    return message;
}

void throwJava(JNIEnv* env, const char* className, const std::string& message) {
    jclass cls = env->FindClass(className);
    if (cls) env->ThrowNew(cls, message.c_str());
}

// --- Callbacks (NativeBridge.Callbacks) ----------------------------------------

class JavaCallbacks : public IProgressObserver {
public:
    JavaCallbacks(JNIEnv* env, jobject callbacks) : env_(env), cb_(callbacks) {
        jclass cls = env->GetObjectClass(callbacks);
        isCancelled_ = env->GetMethodID(cls, "isCancelled", "()Z");
        openSource_ = env->GetMethodID(cls, "openSource", "(I)Ljava/io/InputStream;");
        onOffer_ = env->GetMethodID(cls, "onOffer", "(Ljava/lang/String;[Ljava/lang/String;[J)Z");
        onSessionStarted_ = env->GetMethodID(cls, "onSessionStarted", "(IJ)V");
        onFileStarted_ = env->GetMethodID(cls, "onFileStarted", "(ILjava/lang/String;J)V");
        onBytes_ = env->GetMethodID(cls, "onBytes", "(JJ)V");
        onFileCompleted_ = env->GetMethodID(cls, "onFileCompleted", "(ILjava/lang/String;Ljava/lang/String;)V");
        onError_ = env->GetMethodID(cls, "onError", "(Ljava/lang/String;)V");
        env->DeleteLocalRef(cls);
    }

    CancelToken& token() { return cancel_; }

    // Mirrors the Java cancel flag into the C++ token. Also called after any
    // I/O failure so "socket closed by cancel()" is reported as Cancelled.
    bool syncCancel() {
        if (cancel_.isCancelled()) return true;
        jboolean c = env_->CallBooleanMethod(cb_, isCancelled_);
        if (env_->ExceptionCheck()) takeException(env_);
        if (c) cancel_.cancel();
        return c;
    }

    jobject openSource(int index) {
        jobject stream = env_->CallObjectMethod(cb_, openSource_, static_cast<jint>(index));
        std::string error = takeException(env_);
        if (!error.empty()) throw std::runtime_error(error);
        if (!stream) throw std::runtime_error("no input stream");
        return stream;
    }

    bool onOffer(const protocol::Offer& offer) {
        jclass stringClass = env_->FindClass("java/lang/String");
        jobjectArray names = env_->NewObjectArray(static_cast<jsize>(offer.files.size()), stringClass, nullptr);
        jlongArray sizes = env_->NewLongArray(static_cast<jsize>(offer.files.size()));
        for (std::size_t i = 0; i < offer.files.size(); ++i) {
            jstring n = toJava(env_, protocol::sanitizeFilename(offer.files[i].name));
            env_->SetObjectArrayElement(names, static_cast<jsize>(i), n);
            env_->DeleteLocalRef(n);
            auto size = static_cast<jlong>(offer.files[i].size);
            env_->SetLongArrayRegion(sizes, static_cast<jsize>(i), 1, &size);
        }
        jstring sender = toJava(env_, offer.senderName);
        jboolean accepted = env_->CallBooleanMethod(cb_, onOffer_, sender, names, sizes);
        bool failed = env_->ExceptionCheck();
        if (failed) takeException(env_);
        env_->DeleteLocalRef(sender);
        env_->DeleteLocalRef(names);
        env_->DeleteLocalRef(sizes);
        env_->DeleteLocalRef(stringClass);
        return !failed && accepted;
    }

    void onProgress(int) override {}
    void onSessionStarted(std::size_t count, uint64_t total) override {
        env_->CallVoidMethod(cb_, onSessionStarted_, static_cast<jint>(count), static_cast<jlong>(total));
        takeException(env_);
    }
    void onFileStarted(std::size_t index, const std::string& name, uint64_t size) override {
        jstring n = toJava(env_, name);
        env_->CallVoidMethod(cb_, onFileStarted_, static_cast<jint>(index), n, static_cast<jlong>(size));
        takeException(env_);
        env_->DeleteLocalRef(n);
    }
    void onBytesTransferred(uint64_t done, uint64_t total) override {
        env_->CallVoidMethod(cb_, onBytes_, static_cast<jlong>(done), static_cast<jlong>(total));
        takeException(env_);
        syncCancel();
    }
    void onFileCompleted(std::size_t index, const std::string& name, const std::string& path, bool) override {
        jstring n = toJava(env_, name);
        jstring p = toJava(env_, path);
        env_->CallVoidMethod(cb_, onFileCompleted_, static_cast<jint>(index), n, p);
        takeException(env_);
        env_->DeleteLocalRef(n);
        env_->DeleteLocalRef(p);
    }
    void onError(const std::string& message) override {
        jstring m = toJava(env_, message);
        env_->CallVoidMethod(cb_, onError_, m);
        takeException(env_);
        env_->DeleteLocalRef(m);
    }

private:
    JNIEnv* env_;
    jobject cb_;
    jmethodID isCancelled_, openSource_, onOffer_, onSessionStarted_, onFileStarted_, onBytes_, onFileCompleted_, onError_;
    CancelToken cancel_;
};

// --- Streams ---------------------------------------------------------------------

class JniByteStream : public IByteStream {
public:
    JniByteStream(JNIEnv* env, jobject in, jobject out, std::string description, JavaCallbacks& callbacks)
        : env_(env), in_(in), out_(out), description_(std::move(description)), callbacks_(callbacks) {
        jclass inClass = env->FindClass("java/io/InputStream");
        jclass outClass = env->FindClass("java/io/OutputStream");
        read_ = env->GetMethodID(inClass, "read", "([BII)I");
        write_ = env->GetMethodID(outClass, "write", "([BII)V");
        flush_ = env->GetMethodID(outClass, "flush", "()V");
        env->DeleteLocalRef(inClass);
        env->DeleteLocalRef(outClass);
        buffer_ = env->NewByteArray(kChunk);
    }
    ~JniByteStream() override { env_->DeleteLocalRef(buffer_); }

    std::size_t readSome(void* buffer, std::size_t length) override {
        jint want = static_cast<jint>(std::min<std::size_t>(length, kChunk));
        jint n = env_->CallIntMethod(in_, read_, buffer_, 0, want);
        check("read");
        if (n <= 0) return 0;   // -1 = EOF
        env_->GetByteArrayRegion(buffer_, 0, n, static_cast<jbyte*>(buffer));
        return static_cast<std::size_t>(n);
    }

    void writeAll(const void* buffer, std::size_t length) override {
        auto* p = static_cast<const jbyte*>(buffer);
        while (length > 0) {
            jint n = static_cast<jint>(std::min<std::size_t>(length, kChunk));
            env_->SetByteArrayRegion(buffer_, 0, n, p);
            env_->CallVoidMethod(out_, write_, buffer_, 0, n);
            check("write");
            p += n;
            length -= static_cast<std::size_t>(n);
        }
    }

    void flush() override {
        env_->CallVoidMethod(out_, flush_);
        check("flush");
    }

    std::string describe() const override { return description_; }

private:
    void check(const char* op) {
        std::string error = takeException(env_);
        if (error.empty()) return;
        callbacks_.syncCancel();
        throw TransportError(std::string(op) + " failed: " + error);
    }

    JNIEnv* env_;
    jobject in_, out_;
    std::string description_;
    JavaCallbacks& callbacks_;
    jmethodID read_, write_, flush_;
    jbyteArray buffer_;
};

class JniFileSource : public IFileSource {
public:
    JniFileSource(JNIEnv* env, jobject stream) : env_(env), stream_(stream) {
        jclass inClass = env->FindClass("java/io/InputStream");
        read_ = env->GetMethodID(inClass, "read", "([BII)I");
        close_ = env->GetMethodID(inClass, "close", "()V");
        env->DeleteLocalRef(inClass);
        buffer_ = env->NewByteArray(kChunk);
    }
    ~JniFileSource() override {
        env_->CallVoidMethod(stream_, close_);
        takeException(env_);
        env_->DeleteLocalRef(buffer_);
        env_->DeleteLocalRef(stream_);
    }
    std::size_t read(char* buffer, std::size_t length) override {
        // InputStream.read may legally return fewer bytes; loop until we have
        // data or EOF so the sender never mistakes a short read for EOF.
        for (;;) {
            jint want = static_cast<jint>(std::min<std::size_t>(length, kChunk));
            jint n = env_->CallIntMethod(stream_, read_, buffer_, 0, want);
            std::string error = takeException(env_);
            if (!error.empty()) throw std::runtime_error(error);
            if (n < 0) return 0;
            if (n == 0) continue;
            env_->GetByteArrayRegion(buffer_, 0, n, reinterpret_cast<jbyte*>(buffer));
            return static_cast<std::size_t>(n);
        }
    }

private:
    JNIEnv* env_;
    jobject stream_;
    jmethodID read_, close_;
    jbyteArray buffer_;
};

jint resultCode(TransferStatus s) {
    switch (s) {
        case TransferStatus::Success: return 0;
        case TransferStatus::Rejected: return 1;
        case TransferStatus::Cancelled: return 2;
        case TransferStatus::TransportFailed: return 3;
        case TransferStatus::ProtocolFailed: return 4;
        case TransferStatus::LocalFileError: return 5;
        case TransferStatus::IntegrityFailed: return 6;
    }
    return 4;
}

constexpr jint kPullRefused = 7;   // NativeBridge.RESULT_PULL_REFUSED

jobject makeResult(JNIEnv* env, TransferStatus status, const std::string& message, std::size_t files,
                   bool tokenMatched, const std::string& sender, jint codeOverride = -1) {
    jclass cls = env->FindClass(kResultClass);
    if (!cls) return nullptr;   // ClassNotFoundError already pending
    jmethodID ctor = env->GetMethodID(cls, "<init>", "(ILjava/lang/String;IZLjava/lang/String;)V");
    jstring m = toJava(env, message);
    jstring s = toJava(env, sender);
    jobject result = env->NewObject(cls, ctor, codeOverride >= 0 ? codeOverride : resultCode(status), m, static_cast<jint>(files),
                                    static_cast<jboolean>(tokenMatched), s);
    env->DeleteLocalRef(m);
    env->DeleteLocalRef(s);
    env->DeleteLocalRef(cls);
    return result;
}

// Shared by sendFiles and sendFilesOnPull. pull = this phone accepted the connection and first
// checks the peer's PULL token (see Protocol.hpp).
jobject sendCommon(JNIEnv* env, bool pull, jobject in, jobject out, jstring link, jstring senderName, jstring token,
                   jobjectArray names, jobjectArray mimes, jlongArray sizes, jobject callbacks) {
    if (!in || !out || !names || !sizes || !callbacks) {
        throwJava(env, "java/lang/NullPointerException", "sendFiles: null argument");
        return nullptr;
    }
    JavaCallbacks cb(env, callbacks);
    const jsize count = env->GetArrayLength(names);
    if (env->GetArrayLength(sizes) != count || (mimes && env->GetArrayLength(mimes) != count)) {
        throwJava(env, "java/lang/IllegalArgumentException", "names/mimes/sizes length mismatch");
        return nullptr;
    }
    std::vector<jlong> sizeValues(static_cast<std::size_t>(count));
    env->GetLongArrayRegion(sizes, 0, count, sizeValues.data());

    std::vector<OutgoingFile> files;
    for (jsize i = 0; i < count; ++i) {
        if (sizeValues[i] < 0) {
            throwJava(env, "java/lang/IllegalArgumentException", "file size unknown (negative)");
            return nullptr;
        }
        OutgoingFile f;
        auto n = static_cast<jstring>(env->GetObjectArrayElement(names, i));
        f.name = toUtf8(env, n);
        env->DeleteLocalRef(n);
        if (mimes) {
            auto m = static_cast<jstring>(env->GetObjectArrayElement(mimes, i));
            if (m) f.mime = toUtf8(env, m);
            env->DeleteLocalRef(m);
        }
        f.size = static_cast<uint64_t>(sizeValues[i]);
        f.open = [env, &cb, i]() -> std::unique_ptr<IFileSource> {
            return std::make_unique<JniFileSource>(env, cb.openSource(static_cast<int>(i)));
        };
        files.push_back(std::move(f));
    }

    JniByteStream stream(env, in, out, toUtf8(env, link), cb);
    SendOptions options;
    options.senderName = toUtf8(env, senderName);
    options.token = toUtf8(env, token);
    cb.syncCancel();
    if (!pull) {
        SendResult r = sendFiles(stream, files, options, &cb, &cb.token());
        if (r.status != TransferStatus::Success && cb.syncCancel()) r.status = TransferStatus::Cancelled;
        return makeResult(env, r.status, r.message, r.filesCompleted, false, "");
    }
    PullSendResult r = sendFilesOnPull(stream, files, options, &cb, &cb.token());
    if (r.pullRefused) return makeResult(env, r.status, r.message, 0, false, r.receiverName, kPullRefused);
    if (r.status != TransferStatus::Success && cb.syncCancel()) r.status = TransferStatus::Cancelled;
    return makeResult(env, r.status, r.message, r.filesCompleted, true, r.receiverName);
}

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL
Java_com_example_myapplication_transfer_NativeBridge_protocolInfo(JNIEnv* env, jclass) {
    std::string json = std::string("{\"protocol\":") + std::to_string(protocol::kVersion) +
                       ",\"uuid\":\"" + protocol::kBluetoothServiceUuid + "\",\"aid\":\"" + protocol::kHceAidHex +
                       "\",\"serviceName\":\"" + protocol::kBluetoothServiceName + "\"}";
    return toJava(env, json);
}

JNIEXPORT jstring JNICALL
Java_com_example_myapplication_transfer_NativeBridge_sanitizeFilename(JNIEnv* env, jclass, jstring name) {
    return toJava(env, protocol::sanitizeFilename(toUtf8(env, name)));
}

// Validates a handover payload (from an NFC tap) and returns it in canonical form.
JNIEXPORT jstring JNICALL
Java_com_example_myapplication_transfer_NativeBridge_parseHandover(JNIEnv* env, jclass, jstring json) {
    try {
        return toJava(env, connectionInfoToJson(parseConnectionInfo(toUtf8(env, json))));
    } catch (const std::exception& e) {
        throwJava(env, "java/lang/IllegalArgumentException", e.what());
        return nullptr;
    }
}

JNIEXPORT jobject JNICALL
Java_com_example_myapplication_transfer_NativeBridge_sendFiles(JNIEnv* env, jclass, jobject in, jobject out,
                                                               jstring link, jstring senderName, jstring token,
                                                               jobjectArray names, jobjectArray mimes,
                                                               jlongArray sizes, jobject callbacks) {
    return sendCommon(env, false, in, out, link, senderName, token, names, mimes, sizes, callbacks);
}

JNIEXPORT jobject JNICALL
Java_com_example_myapplication_transfer_NativeBridge_sendFilesOnPull(JNIEnv* env, jclass, jobject in, jobject out,
                                                                     jstring link, jstring senderName, jstring token,
                                                                     jobjectArray names, jobjectArray mimes,
                                                                     jlongArray sizes, jobject callbacks) {
    return sendCommon(env, true, in, out, link, senderName, token, names, mimes, sizes, callbacks);
}

JNIEXPORT jobject JNICALL
Java_com_example_myapplication_transfer_NativeBridge_receiveFiles(JNIEnv* env, jclass, jobject in, jobject out,
                                                                  jstring link, jstring destDir,
                                                                  jstring expectedToken, jlong maxTotalBytes,
                                                                  jobject callbacks) {
    if (!in || !out || !destDir || !callbacks) {
        throwJava(env, "java/lang/NullPointerException", "receiveFiles: null argument");
        return nullptr;
    }
    JavaCallbacks cb(env, callbacks);
    ReceiveOptions options;
    options.destinationDir = std::filesystem::u8path(toUtf8(env, destDir));
    options.expectedToken = toUtf8(env, expectedToken);
    if (maxTotalBytes > 0) {
        options.limits.maxTotalBytes = static_cast<uint64_t>(maxTotalBytes);
        options.limits.maxFileBytes = static_cast<uint64_t>(maxTotalBytes);
    }
    options.acceptOffer = [&cb](const protocol::Offer& offer) { return cb.onOffer(offer); };

    JniByteStream stream(env, in, out, toUtf8(env, link), cb);
    ReceiveResult r = receiveFiles(stream, options, &cb, &cb.token());
    if (r.status != TransferStatus::Success && cb.syncCancel()) r.status = TransferStatus::Cancelled;
    return makeResult(env, r.status, r.message, r.files.size(), r.tokenMatched, r.senderName);
}

}  // extern "C"
