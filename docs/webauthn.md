# Web Authentication (passkeys and security keys)

Nordstjernen implements the [Web Authentication](https://www.w3.org/TR/webauthn-3/)
API: `navigator.credentials.create()` / `get()` with `publicKey` options,
`PublicKeyCredential` and the `AuthenticatorAttestationResponse` /
`AuthenticatorAssertionResponse` objects. Sites use it for passkeys,
security keys, and to decide which sign-in options to show (GitHub hides
its Google, Apple and passkey buttons from browsers without it).

## Where things run

| Layer | File | Job |
|---|---|---|
| Page API | `data/js/polyfills.js` (`installWebAuthn`) | WebIDL surface, option normalisation (§5.1.3 / §5.1.4), timeouts, `AbortSignal`, `toJSON()`, `parse*OptionsFromJSON()`, `getPublicKey()` from the attestation's COSE key |
| Engine | `src/js_webauthn.c` | Accepts requests only from the top-level document's realm, attaches the document origin the engine navigated to, re-serialises the options with QuickJS's own JSON (page code cannot inject into the message) and settles the promise when the shell answers |
| Transport | `src/renderer_serve.c`, `src/rproc_http.c` | `X-WebAuthn: 1` on render/tick replies → shell `GET /webauthn-poll`; answers go back on `POST /webauthn-deliver` |
| Shell | `src/gtk/ns_webauthn.c` | The trust boundary (below), one ceremony per tab, cancelled on navigation and tab close |
| Authenticator | `src/gtk/webauthn_win.c` | Windows: `webauthn.dll`, loaded at runtime — Windows Hello, security keys over USB/NFC/BLE, and phones (hybrid) |

The sandboxed renderer never talks to an authenticator. The shell treats
everything the renderer sends as untrusted:

- the request's origin must be the origin of the page the tab is showing,
  and be `https:` (or `http://localhost`);
- the effective domain must be a domain, not an IP address;
- the RP ID must equal the page's host or be a registrable-domain suffix of
  it that is not a public suffix (checked with libpsl);
- `clientDataJSON` — type, challenge, origin, `crossOrigin: false` — is built
  by the shell from its own view of the page, so a compromised renderer
  cannot obtain an assertion for another site.

Frames are refused (`NotAllowedError`) until the
`publickey-credentials-get` / `-create` permissions policy is implemented.
Conditional mediation (passkey autofill) is not supported, so
`isConditionalMediationAvailable()` resolves `false`.

## Platforms

- **Windows 10 1903+**: full support through the system WebAuthn API.
  `isUserVerifyingPlatformAuthenticatorAvailable()` reports Windows Hello.
- **Linux, BSD, macOS**: the API is present and requests reject with
  `NotAllowedError` until a backend lands (libfido2 for USB security keys;
  AuthenticationServices on macOS, which needs Apple's
  `com.apple.developer.web-browser.public-key-credential` entitlement).
- **Headless**: no authenticator; requests run until their timeout.

## Trying it

<https://webauthn.io> registers and signs in with a passkey;
<https://github.com/login> shows "Continue with Google / Apple" and
"Sign in with a passkey" once the API is present.
