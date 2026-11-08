// ============================================================================
//  Backend del portapapeles para Symbian: el portapapeles del SISTEMA (CClipboard),
//  compartido con el navegador, las notas y demas apps (todas usan el componente de
//  texto plano KClipboardUidTypePlainText). Reusa la RFs de la app (CCoeEnv). El
//  texto del sistema es UCS-2; el editor de scripts filtra a ASCII, asi que aca
//  hacemos un widen/narrow simple byte<->TChar (suficiente para codigo).
// ============================================================================
#if defined(W3D_SYMBIAN)

#include "W3dClipboard.h"
#include <e32std.h>
#include <baclipb.h>    // CClipboard
#include <txtetext.h>   // CPlainText (usa KClipboardUidTypePlainText por dentro)
#include <coemain.h>    // CCoeEnv::Static()->FsSession()

namespace w3dEngine {

static RFs& ClipFs() { return CCoeEnv::Static()->FsSession(); }

// --- ESCRIBIR (funcion que puede LEAVE; se llama bajo TRAP) ---
static void ClipSetL(const TDesC& aText) {
    CClipboard* cb = CClipboard::NewForWritingLC(ClipFs());   // abre el store del sistema para escribir
    CPlainText* txt = CPlainText::NewL();
    CleanupStack::PushL(txt);
    txt->InsertL(0, aText);
    txt->CopyToStoreL(cb->Store(), cb->StreamDictionary(), 0, txt->DocumentLength());
    CleanupStack::PopAndDestroy(txt);
    cb->CommitL();                                            // IMPRESCINDIBLE: sin esto no se publica
    CleanupStack::PopAndDestroy(cb);
}

void W3dClipboardSet(const std::string& s) {
    if (s.empty()) return;
    HBufC* buf = HBufC::New((TInt)s.size());
    if (!buf) return;
    TPtr p = buf->Des();
    for (std::string::size_type i = 0; i < s.size(); i++)
        p.Append((TChar)(TUint8)s[i]);                       // widen byte->TChar (ASCII exacto; latin-ish arriba de 127)
    TRAPD(err, ClipSetL(*buf));
    (void)err;                                               // si el sistema no deja escribir, queda mudo (no rompe)
    delete buf;
}

// --- LEER (funcion que puede LEAVE; devuelve el HBufC del texto pegado o NULL) ---
static HBufC* ClipGetL() {
    CClipboard* cb = CClipboard::NewForReadingLC(ClipFs());   // abre el store del sistema para leer
    CPlainText* txt = CPlainText::NewL();
    CleanupStack::PushL(txt);
    HBufC* result = NULL;
    TInt len = txt->PasteFromStoreL(cb->Store(), cb->StreamDictionary(), 0);  // 0 chars o LEAVE si no hay texto plano
    if (len > 0) {
        result = HBufC::NewL(txt->DocumentLength());
        TPtr rp = result->Des();
        txt->Extract(rp, 0, txt->DocumentLength());
    }
    CleanupStack::PopAndDestroy(txt);
    CleanupStack::PopAndDestroy(cb);
    return result;
}

std::string W3dClipboardGet() {
    std::string out;
    HBufC* result = NULL;
    TRAPD(err, result = ClipGetL());                         // si no hay texto en el portapapeles: LEAVE -> result NULL
    (void)err;
    if (result) {
        TPtr p = result->Des();
        for (TInt i = 0; i < p.Length(); i++) {
            TChar c = p[i];
            if ((TUint)c < 128) out += (char)(TUint)c;       // solo ASCII (el editor de scripts es ASCII)
        }
        delete result;
    }
    return out;
}

} // namespace w3dEngine

#endif // W3D_SYMBIAN
