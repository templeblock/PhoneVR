#include <jni.h>
#include <media/NdkMediaCodec.h>
#include <android/native_window_jni.h>
#include <unistd.h>

#include "Eigen"
#include "PVRRenderer.h"
#include "PVRSockets.h"

#include "media/NdkMediaExtractor.h"
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <queue>
#include <sys/stat.h>

using namespace std;
using namespace Eigen;
using namespace gvr;
using namespace PVR;

#define JNI_VERS JNI_VERSION_1_6
#define FUNC(type, func) extern "C" JNIEXPORT type JNICALL Java_viritualisres_phonevr_Wrap_##func
#define SUB(func) FUNC(void, func)

namespace {
    JavaVM *jVM;
    jclass javaWrap;

    float newAcc[3];
    uint16_t vPort = 0;


    int maxWidth, maxHeight;
    vector<uint8_t> vHeader;

    array<float, 16> GetMat3(float m[4][4]) {
        array<float, 16> arr;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; ++j)
                arr[i * 3 + j] = m[i][j];
        return arr;
    }

    ANativeWindow *window = nullptr;
    AMediaCodec *codec;
    thread *mediaThr;
    int64_t vOutPts = -1;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    PVR_DB("JNI initiating...");
    jVM = vm;
    JNIEnv *env;
    jVM->GetEnv((void **) &env, JNI_VERS);
    javaWrap = (jclass) env->NewGlobalRef(env->FindClass("viritualisres/phonevr/Wrap"));
    return JNI_VERS;
}

void callJavaMethod(const char *name) {
    PVR_DB("JNI callJavaMethod: Calling " + to_string(name));
    bool isMainThread = true;
    JNIEnv *env;
    if (jVM->GetEnv((void **) &env, JNI_VERS) != JNI_OK) {
        isMainThread = false;
        jVM->AttachCurrentThread(&env, nullptr);
    }
    env->CallStaticVoidMethod(javaWrap, env->GetStaticMethodID(javaWrap, name, "()V"));
    if (!isMainThread)
        jVM->DetachCurrentThread();
}

/////////////////////////////////////// discovery ////////////////////////////////////////////////
SUB(startAnnouncer)(JNIEnv *env, jclass, jstring jIP, jint port) {
    PVR_DB("JNI startAnnouncer: %s"+to_string(jIP)+":%d"+ to_string(port));
    auto ip = env->GetStringUTFChars(jIP, nullptr);
    PVRStartAnnouncer(ip, port, [] {
        callJavaMethod("segueToGame");
    }, [](uint8_t *headerBuf, size_t len) {
        vHeader = vector<uint8_t>(headerBuf, headerBuf + len);
    }, [] {
        callJavaMethod("unwindToMain");
    });
    env->ReleaseStringUTFChars(jIP, ip);
}

SUB(stopAnnouncer)() {
    PVR_DB("JNI stopAnnouncer");
    PVRStopAnnouncer();
}

////////////////////////////////////// orientation ///////////////////////////////////////////////
SUB(startSendSensorData)(JNIEnv *env, jclass, jint port) {
    PVR_DB("JNI startSendSensorData %d" + to_string(port));

    PVRStartSendSensorData(port, [](float *quat, float *acc) {
        if (gvrApi) {
            auto gmat = gvrApi->GetHeadSpaceFromStartSpaceRotation(GvrApi::GetTimePointNow());
            Quaternionf equat(Map<Matrix3f>(GetMat3(gmat.m).data()));

            quat[0] = equat.w();
            quat[1] = equat.x();
            quat[2] = equat.y();
            quat[3] = equat.z();
            memcpy(acc, newAcc, 3 * 4);
            return true;
        }
        return false;
    });
}

SUB(setAccData)(JNIEnv *env, jclass, jfloatArray jarr) {
    //PVR_DB("JNI setAccData");
    auto *accArr = env->GetFloatArrayElements(jarr, nullptr);
    memcpy(newAcc, accArr, 3 * 4);
    env->ReleaseFloatArrayElements(jarr, accArr, JNI_ABORT);
}

///////////////////////////////////// system control & events /////////////////////////////////////
SUB(createRenderer)(JNIEnv *, jclass, jlong jGvrApi) {
    PVR_DB("JNI createRenderer");
    PVRCreateGVR(reinterpret_cast<gvr_context *>(jGvrApi));
}

SUB(onPause)() {
    PVR_DB("JNI onPause");
    PVRPause();
}

SUB(onTriggerEvent)() {
    PVR_DB("JNI onTriggerEvent");
    PVRTrigger();
}

SUB(onResume)() {
    PVR_DB("JNI onResume");
    PVRResume();
}

///////////////////////////////////// video stream & coding //////////////////////////////////////
SUB(setVStreamPort)(JNIEnv *, jclass, jint port) {
    PVR_DB("JNI setVStreamPort - %d"+ to_string(port));
    vPort = (uint16_t)port;
}

SUB(startStream)() {// cannot pass parameter here or else sigabrit on getting frame (why???)
    PVR_DB("JNI startStream");
    PVRStartReceiveStreams((uint16_t)vPort);
}

SUB(startMediaCodec)(JNIEnv *env, jclass, jobject surface) {
    PVR_DB("JNI startMediaCodec");
    window = ANativeWindow_fromSurface(env, surface);


    auto *fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, maxWidth);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, maxHeight);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_WIDTH, maxWidth);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_HEIGHT, maxHeight);
    //AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, 62);
    //AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, 1000000);
    AMediaFormat_setBuffer(fmt, "csd-0", &vHeader[0], vHeader.size());

    codec = AMediaCodec_createDecoderByType("video/avc");
    auto m = AMediaCodec_configure(codec, fmt, window, nullptr, 0);

    m = AMediaCodec_start(codec);
    AMediaFormat_delete(fmt);

    PVR_DB("JNI MCodec th Setup...");

    mediaThr = new thread([]{
        while (pvrState != PVR_STATE_SHUTDOWN)
        {
            if (PVRIsVidBufNeeded()) //emptyVBufs.size() < 3
            {
                auto idx = AMediaCodec_dequeueInputBuffer(codec, 1000); // 1ms timeout
                if (idx >= 0)
                {
                    size_t bufSz = 0;
                    uint8_t *buf = AMediaCodec_getInputBuffer(codec, (size_t)idx, &bufSz);
                    PVREnqueueVideoBuf({buf, idx, bufSz}); // into emptyVbuf
                    PVR_DB("[MediaCodec th] Getting MCInputMedia Buf[ "+ to_string(bufSz)+ "] @ idx:" + to_string(idx) + ". into emptyVbuf");
                }
            }

            auto fBuf = PVRPopVideoBuf(); //filledVBuf
            if (fBuf.idx != -1)
            {
                AMediaCodec_queueInputBuffer(codec, (size_t)fBuf.idx, 0, fBuf.pktSz, fBuf.pts, 0); //AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM
                PVR_DB("[MediaCodec th] Getting filledVBuf Buf[ "+ to_string(fBuf.pktSz)+ "] @ idx:" + to_string(fBuf.idx) + ", pts:" + to_string(fBuf.pts) +". into MCqInputBuf");
            }

            AMediaCodecBufferInfo info;
            auto outIdx = AMediaCodec_dequeueOutputBuffer(codec, &info, 1000); // 1ms timeout
            if (outIdx >= 0)
            {
                PVR_DB("[MediaCodec th] Output: " + to_string(AMediaFormat_toString(AMediaCodec_getOutputFormat(codec))));

                /*int result_code = mkdir("/data/data/viritualisres.phonevr/files/", 0777);
                PVR_DB("[MediaCodec th] Dir path /data/data/viritualisres.phonevr/files/ " + to_string(result_code));

                auto *file = fopen("/data/data/viritualisres.phonevr/files/mystream.h264","wb");

                if(file) {
                    auto buf = AMediaCodec_getOutputBuffer(codec, outIdx,
                                                           reinterpret_cast<size_t *>(&info.size));
                    fwrite(buf, sizeof(uint8_t), ((info.size)/sizeof(uint8_t)), file);
                    fclose(file);
                    PVR_DB("[MediaCodec th] Wrote stream to file /storage/emulated/0/etc/mystream.h264");
                } else {
                    PVR_DB("[MediaCodec th] File open failed /storage/emulated/0/etc/mystream.h264");
                }*/

                bool render = info.size != 0;
                auto status = AMediaCodec_releaseOutputBuffer(codec, (size_t) outIdx, render);
                if (render)
                    vOutPts = info.presentationTimeUs;
                PVR_DB("[MediaCodec th] MCreleaseOutputBuffer Buf @ idx:" + to_string(outIdx) + ", pts:" + (render?("Rendered "+to_string(vOutPts)):"NotRendered") + ", Status: " + to_string(status) + ", Outsize:" + to_string(info.size));
            }
        }
    });
    pvrState = PVR_STATE_RUNNING;
}

FUNC(jlong, vFrameAvailable)(JNIEnv *) {
    auto pts = vOutPts;
    vOutPts = -1;
    return pts;
}

SUB(stopAll)(JNIEnv *) {
    PVR_DB("JNI stopAll");
    pvrState = PVR_STATE_SHUTDOWN;
    PVRStopStreams();
    PVRDestroyGVR();

    mediaThr->join();
    delete mediaThr;

    AMediaCodec_stop(codec);
    AMediaCodec_delete(codec);
    ANativeWindow_release(window);
    pvrState = PVR_STATE_IDLE;
}

//////////////////////////////////////////// drawing //////////////////////////////////////////////
FUNC(jint, initSystem)(JNIEnv *, jclass, jint x, jint y, jint resMul, jfloat offFov, jboolean reproj, jboolean debug) {
    int w = (x > y ? x : y), h = (x > y ? y : x);
    maxWidth = min(w / 8, resMul * w / h) * 8; // keep pixel aspect ratio (does not influence image aspect ratio)
    maxHeight = min(h / 8, resMul) * 8;
    return PVRInitSystem(maxWidth, maxHeight, offFov, reproj, debug);
}

SUB(drawFrame)(JNIEnv *env, jclass, jlong pts) {
    PVRRender(pts);
}

