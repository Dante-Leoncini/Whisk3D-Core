// ============================================================================
//  Backend de salida Symbian (N95): CMdaAudioOutputStream en su propio hilo, con
//  doble-buffer. Cada vez que el stream copio un buffer, mezclamos el siguiente con
//  el mixer del Core (W3dAudioMix) y lo escribimos. Modelo tomado de la demo de audio
//  del port de Quake III (q3rev: audiostream.cpp), que anda en el telefono.
//
//  Exclusion con el hilo principal (play/stop de voces) via RMutex.
//
//  .mmp: agregar  LIBRARY mediaclientaudiostream.lib euser.lib  (CMdaAudioOutputStream +
//  RThread/RMutex). Compilar este archivo solo con -DW3D_ENABLE_AUDIO.
// ============================================================================
#if defined(W3D_ENABLE_AUDIO) && defined(W3D_SYMBIAN)

#include "W3dAudio.h"
#include <e32base.h>
#include <e32std.h>
#include <MdaAudioOutputStream.h>
#include <mda/common/audio.h>

namespace w3dEngine {

static RThread s_thread;
static RMutex  s_mutex;
static TBool   s_running = EFalse;
static volatile TInt s_stop = 0;
static TInt    s_rate = 22050;

// doble buffer estatico: 1024 frames stereo 16-bit (~46 ms @ 22050)
static const TInt KBufBytes = 4096;
static TUint8  s_buf[2][KBufBytes];
static TInt    s_bufIdx = 0;

class CW3dAudioStream : public CBase, public MMdaAudioOutputStreamCallback {
public:
    static CW3dAudioStream* NewL(TInt aRate);
    ~CW3dAudioStream();
    void MaoscOpenComplete(TInt aError);
    void MaoscBufferCopied(TInt aError, const TDesC8& aBuffer);
    void MaoscPlayComplete(TInt aError);
    void WriteNext();
private:
    void ConstructL();
    TInt iRate;
    CMdaAudioOutputStream* iStream;
    TMdaAudioDataSettings  iSettings;
    TPtrC8 iDes;
};

CW3dAudioStream* CW3dAudioStream::NewL(TInt aRate) {
    CW3dAudioStream* self = new (ELeave) CW3dAudioStream();
    self->iRate = aRate;
    CleanupStack::PushL(self);
    self->ConstructL();
    CleanupStack::Pop();
    return self;
}
void CW3dAudioStream::ConstructL() {
    iStream = CMdaAudioOutputStream::NewL(*this);
    iStream->Open(&iSettings);
}
CW3dAudioStream::~CW3dAudioStream() { delete iStream; }

void CW3dAudioStream::MaoscOpenComplete(TInt aError) {
    if (aError != KErrNone) return;
    TInt sr = TMdaAudioDataSettings::ESampleRate22050Hz;
    if      (iRate == 11025) sr = TMdaAudioDataSettings::ESampleRate11025Hz;
    else if (iRate == 44100) sr = TMdaAudioDataSettings::ESampleRate44100Hz;
    else if (iRate == 48000) sr = TMdaAudioDataSettings::ESampleRate48000Hz;
    iStream->SetAudioPropertiesL(sr, TMdaAudioDataSettings::EChannelsStereo);
    iStream->SetVolume(iStream->MaxVolume());
    iStream->SetPriority(EMdaPriorityMax, EMdaPriorityPreferenceTimeAndQuality);
    WriteNext();
    WriteNext(); // encolar dos buffers para arrancar sin cortes
}
void CW3dAudioStream::MaoscBufferCopied(TInt aError, const TDesC8&) {
    if (aError == KErrNone) WriteNext();
}
void CW3dAudioStream::MaoscPlayComplete(TInt) {}

void CW3dAudioStream::WriteNext() {
    if (s_stop) { CActiveScheduler::Stop(); return; } // shutdown limpio: cortamos el pump
    TUint8* buf = s_buf[s_bufIdx];
    s_bufIdx ^= 1;
    s_mutex.Wait();
    W3dAudioMix((short*)buf, KBufBytes / 4);
    s_mutex.Signal();
    iDes.Set(buf, KBufBytes);
    TRAP_IGNORE(iStream->WriteL(iDes));
}

static CW3dAudioStream* s_streamObj = 0;
static TInt AudioThreadFunc(TAny*) {
    CTrapCleanup* cleanup = CTrapCleanup::New();
    if (!cleanup) return KErrNoMemory;
    CActiveScheduler* sched = new CActiveScheduler;
    if (sched) {
        CActiveScheduler::Install(sched);
        TRAP_IGNORE(s_streamObj = CW3dAudioStream::NewL(s_rate));
        CActiveScheduler::Start(); // corre hasta que WriteNext haga ::Stop()
        delete s_streamObj; s_streamObj = 0;
        CActiveScheduler::Install(NULL);
        delete sched;
    }
    delete cleanup;
    return KErrNone;
}

bool W3dAudioBackendInit(int rate) {
    s_rate = rate; s_stop = 0; s_bufIdx = 0;
    if (s_mutex.CreateLocal() != KErrNone) return false;
    _LIT(KName, "W3dAudioThread");
    if (s_thread.Create(KName, AudioThreadFunc, 32768, NULL, NULL) != KErrNone) {
        s_mutex.Close();
        return false;
    }
    s_thread.Resume();
    s_running = ETrue;
    return true;
}

void W3dAudioBackendShutdown() {
    if (!s_running) return;
    s_stop = 1;
    TRequestStatus st;
    s_thread.Logon(st);
    User::WaitForRequest(st); // espera a que el hilo termine solo (WriteNext -> ::Stop -> return)
    s_thread.Close();
    s_mutex.Close();
    s_running = EFalse;
}

void W3dAudioBackendLock()   { s_mutex.Wait(); }
void W3dAudioBackendUnlock() { s_mutex.Signal(); }

} // namespace w3dEngine

#endif // W3D_ENABLE_AUDIO && W3D_SYMBIAN
