#include <jni.h>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include "stdio.h"
#include <pthread.h>

#define _LOG_ENABLE_
static const char *TAG = "SerialPortJNI_C";
#ifdef _LOG_ENABLE_
#include "android/log.h"
#define LOGI(fmt, args...) __android_log_print(ANDROID_LOG_INFO,  TAG, fmt, ##args)
#define LOGD(fmt, args...) __android_log_print(ANDROID_LOG_DEBUG, TAG, fmt, ##args)
#define LOGE(fmt, args...) __android_log_print(ANDROID_LOG_ERROR, TAG, fmt, ##args)
#else
#define LOGI(fmt, args...)
#define LOGD(fmt, args...)
#define LOGE(fmt, args...)
#endif

#define uint8 unsigned char
/*透明编解码------------------------------------------------------------------------------------*/
static short Decode(uint8* buf,short len)
{
    uint8 v80,v81;short i=0,j=0;
    for(;;)
    {
        v80=buf[i];v81=buf[i+1];
        if((v80==0x7e)||(i>=len))break;
        if(v80!=0x7d){buf[j++]=v80;i++;}
        else
        {
            if(v81==0x5e){buf[j++]=0x7e;i+=2;}
            else if(v81==0x5d){buf[j++]=0x7d;i+=2;}
            else {buf[j++]='?';i++;}
        }
    }return j;
}

static short Encode(uint8 inbuf[],uint8 outbuf[],short len_in)
{
    uint8 v8,chksum=0;short i,j,len=len_in;
    for(i=0;i<len;i++)chksum+=inbuf[i];
    inbuf[len]=chksum;len++;//chksum
    j=0;for(i=0;i<len;i++)
    {
        v8=inbuf[i];
        if(v8==0x7e){outbuf[j++]=0x7d;outbuf[j++]=0x5e;}
        else if(v8==0x7d){outbuf[j++]=0x7d;outbuf[j++]=0x5d;}
        else outbuf[j++]=v8;
    }outbuf[j++]=0x7e;return j;
}

/*接收包处理*/
//JNIEnv* jniEnv = NULL;
static JavaVM* gJavaVM=NULL;
static jclass  gJclazz=NULL;
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved)
{
    JNIEnv* env = NULL;jint result = -1;
    if (vm->GetEnv((void**) &env, JNI_VERSION_1_4) != JNI_OK) {
        LOGE("jni_load failed\n");return result;
    }
    result = JNI_VERSION_1_4;return result;
}

static void send_data_to_java(short appid,short msgid,jbyte status,short length,jbyte rxbuf[],JNIEnv* jniEnv,jclass jclazz,jmethodID jmethodid)
{
    if((jniEnv==NULL)||(jclazz==NULL)||(jmethodid==NULL))return;
    jbyteArray jbyteArr; //定义jbyte数组
    jbyteArr = jniEnv->NewByteArray(length);
    jniEnv->SetByteArrayRegion(jbyteArr,0,length,rxbuf);
    jniEnv->CallStaticVoidMethod(jclazz,jmethodid,appid,msgid,status,length,jbyteArr);
    jniEnv->DeleteLocalRef(jbyteArr); //一定要释放内存
//    jniEnv->DeleteLocalRef(jclazz);return;
}

static void fncCmmPrtcl(uint8 *buf, short inlen,JNIEnv* jniEnv,jclass jclazz,jmethodID jmethodid)
{
    short i,len = Decode(buf, inlen);if (len <= 8)return;//长度不足直接抛弃
    uint8 chksum = 0;for (i = 0; i < len - 1; i++)chksum += buf[i];
    LOGD("fnc.......:%d,%d,%d,%d,%d,%d,%d",chksum,buf[len-2],buf[len-1],buf[len+0],buf[len+1],len,inlen);
//    if (chksum != buf[len - 1])return;/*chksum error直接抛弃*/
    short appid=buf[0]|(buf[1]<<8);//appid;
    short msgid=buf[2]|(buf[3]<<8);
    jbyte status=buf[4];//jbyte rxbuf[400];
    short paylen = buf[5] | (buf[6] << 8);
    LOGD("fncCmmPrtcl:%d,%d,%d,%d",appid,msgid,status,paylen);
    send_data_to_java(appid,msgid,status,paylen,(jbyte*)(&buf[7]),jniEnv,jclazz,jmethodid);return;
}

/*队列操作函数--------------------------------------------------------------------------------------*/
#define Q_SUCCESS    0
#define Q_EMPTY     -1
#define RX_BUFFER_SIZE	1024
static int nHeadR,nTailR;
static uint8 RxBufQ[RX_BUFFER_SIZE];
static void QueueInit(void)
{
    nHeadR=0;nTailR=0;return;
}

static short QueueAppend(uint8* Value,short len)
{
    short head,tail,i;uint8* ptr=Value;
    head=nHeadR;tail=nTailR;
    for(i=0;i<len;i++)
    {
        RxBufQ[head++]=*ptr++;
        if(head>=RX_BUFFER_SIZE)head=0;
        if(head!=nTailR)nHeadR=head;
    }
    return Q_SUCCESS;
}

static short QueueRemove(uint8* Value)
{
    short head,tail;head=nHeadR;tail=nTailR;
    if (head == tail )return Q_EMPTY;
    *Value = RxBufQ[tail++];
    if (tail>= RX_BUFFER_SIZE)tail=0;
    nTailR=tail;return Q_SUCCESS;
}

static short QueueInUsed(void)
{
    short head,tail;head=nHeadR;tail=nTailR;
    if(head>=tail)return head-tail;
    else return head + RX_BUFFER_SIZE-tail;
}
/*接收线程处理函数----------------------------------------------------------------------------------*/
static unsigned char    rxEncBuf[1024];
static short            rxEncLen = 0;
static void func_slot_uart(uint8 *buf, short rev,JNIEnv* jniEnv,jclass jclazz,jmethodID jmethodid) {
    int i, len;uint8 v8;
    QueueAppend(buf, rev);
    len = QueueInUsed();
    if (len > 0) {
        int enl = rxEncLen;
        for (i = 0; i < len; i++) {
            if (!(QueueRemove(&v8))) {
                rxEncBuf[enl++] = v8;
                if (v8 != 0x7e) { if (enl >= 1024)enl = 0; }
                else { fncCmmPrtcl(rxEncBuf, enl,jniEnv,jclazz,jmethodid);enl = 0; }
            }rxEncLen = enl;
        }
    }
}

static short bStarted = 0, bStop = 1;
static int fd;
static pthread_t thread;
void *serial_thread(void *arg)
{
    int rev;uint8 buf[401];
    JNIEnv* jniEnv=NULL;jmethodID jmethodid=NULL;
    if(gJavaVM->AttachCurrentThread(&jniEnv, NULL) == JNI_OK) {//Attach主线程
        if ((jniEnv != NULL) && (gJclazz != NULL)) {
            jmethodid = jniEnv->GetStaticMethodID(gJclazz, "recData", "(SSBS[B)V");
        }
    }
    while (!bStop)
    {
        rev = read(fd, (buf), 400);
        if (rev) { func_slot_uart(buf, rev,jniEnv,gJclazz,jmethodid); }
    }
    gJavaVM->DetachCurrentThread();return NULL;
}
/*串口初始化处理----------------------------------------------------------------------------------*/
static jint tbl_bauds[] =
        {
                0, 50, 75, 110, 134, 150, 200, 300, 600, 1200, 1800, 2400,
                4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 500000, 576000, 921600,
                1000000, 1152000, 1500000, 2000000, 2500000, 3000000, 3500000, 4000000,
        };

static speed_t tbl_speeds[] =
        {
                B0, B50, B75, B110, B134, B150, B200, B300, B600, B1200, B1800, B2400,
                B4800, B9600, B19200, B38400, B57600, B115200, B230400, B460800, B500000, B576000,
                B921600, B1000000, B1152000, B1500000, B2000000, B2500000, B3000000, B3500000, B4000000,
        };

static speed_t getBaudrate(jint baudrate)
{
    for (int i = 0; i < sizeof(tbl_bauds) / sizeof(jint); i++)
    {
        if (baudrate == tbl_bauds[i])return tbl_speeds[i];
    }
    return -1;
}

extern "C"
JNIEXPORT jint JNICALL Java_com_jmake_serialport_SerialPortJNI_initProtocol(JNIEnv *env, jobject thiz)
{
    LOGI("InitProtocol\n");
    bStarted = 0;bStop = 1;fd = -1;
    return 0;
}

/*打开UART口*/
static pthread_attr_t attr;
extern "C"
JNIEXPORT jint JNICALL Java_com_jmake_serialport_SerialPortJNI_start
        (JNIEnv *env, jobject thiz, jstring path, jint baudrate, jint flags)
{
    LOGI("start\n");
    if ((bStarted) & (!bStop)){return fd;}
    LOGI("start_serial()\n");
    rxEncLen = 0;QueueInit();
    speed_t speed=getBaudrate(baudrate);

    jboolean iscopy;
    const char *path_utf = env->GetStringUTFChars(path, &iscopy);
    fd = open(path_utf, O_RDWR | /*O_NOCTTY |*/ flags);
    LOGD("start(fd = %d)", fd);
    env->ReleaseStringUTFChars(path, path_utf);
    if (fd <0)
    {
        LOGE("can't open port");
        return fd;
    }

    struct termios cfg;cfmakeraw(&cfg);
    cfg.c_iflag |= IGNPAR; /*ignore parity on input */
    cfg.c_oflag &= ~(OPOST | ONLCR | OLCUC | OCRNL | ONOCR | ONLRET | OFILL);
    cfg.c_cflag = CS8 | CLOCAL | CREAD;
    cfg.c_cc[VMIN] = 1; /* block until 1 char received */
    cfg.c_cc[VTIME] = 0; /*no inter-character timer */

    cfsetispeed(&cfg, speed);
    cfsetospeed(&cfg, speed);
    tcflush(fd, TCIFLUSH);

    if (tcsetattr(fd, TCSANOW, &cfg))
    {
        LOGE("tcsetattr() failed");
        close(fd);return -1;
    }
    env->GetJavaVM(&gJavaVM);
    jclass tmp = env->FindClass("com/jmake/serialport/SerialPortJNI");
    gJclazz = (jclass)env->NewGlobalRef(tmp);
    pthread_attr_init(&attr);bStop = 0;
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&thread, &attr, serial_thread, NULL/*(void *)(&fd)*/);
    bStarted = 1;bStop = 0;return fd;
}

/*停止UART口*/
extern "C"
JNIEXPORT jint JNICALL Java_com_jmake_serialport_SerialPortJNI_stop(JNIEnv *env, jobject thiz)
{
    jint i;if (bStop)return 0;
    LOGD("close-1(fd = %d)", fd);bStop = 1;
    if (fd >= 0)
    {
        pthread_attr_destroy(&attr);
        tcflush(fd, TCIFLUSH);close(fd);
    }
    env->DeleteGlobalRef(gJclazz);
    fd = -1;bStarted = 0;bStop = 1;return 0;
}

/*送数据给UART*/
static int SendBuffer(uint8 *buf, int len)
{
    int txlen = len, txed1 = 0, txed2 = 0;
    if (fd < 0)return 0;
    while (txed2 < txlen)
    {
        txed1 = write(fd, (buf + txed2), txlen - txed2);
        txed2 += txed1;
    }return 0;
}

static uint8 buf0[1536],buf1[2048];
extern "C"
JNIEXPORT jint JNICALL Java_com_jmake_serialport_SerialPortJNI_sendData
        (JNIEnv *env, jobject thiz,jshort appid,jshort msgid,jbyte ack,jshort length,jbyteArray jbytearray) {
    jshort len = 0;
    buf0[len++] = (appid >> 0) & 0xff;buf0[len++] = (appid >> 8) & 0xff;//app_id
    buf0[len++] = (msgid >> 0) & 0xff;buf0[len++] = (msgid >> 8) & 0xff;//msgid
    buf0[len++] = ack;//ack
    buf0[len++] = (length >> 0) & 0xff;buf0[len++] = (length >> 8) & 0xff;//length
    if (length > 0) {
        jbyte *jbbuf = env->GetByteArrayElements(jbytearray, 0);
        for (jshort i = 0; i < length; i++)buf0[len++] = jbbuf[i];
        env->ReleaseByteArrayElements(jbytearray, jbbuf, 0);
    }
    len = Encode(buf0, buf1, len);SendBuffer(buf1, len);
//    LOGD("fncCmmPrtcl..send:%d,%d,%d,%d",appid,msgid,ack,len);
    return 0;
}

/*-------------------------end of file------------------------------------------------------------*/
