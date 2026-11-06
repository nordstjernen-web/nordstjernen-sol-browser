# Nordstjernen for Java

**A web browser for the JVM** — and the library it is built on. `org.nordstjernen.app.Browser`
is a standalone, pure-Java desktop browser (Swing chrome over the Nordstjernen
rendering engine); the same jar doubles as an embedding library for fetching,
laying out, scripting and rendering web pages from your own Java code.

**Requires JDK 21.**

## Run the browser

The Java browser is a Swing application with GTK-shell-style chrome — back,
forward, reload, home, a URL bar with a transport-security indicator, and a
status bar. It drives a separate `nordstjernen-renderer` process (no native
engine is loaded into the JVM), so an engine crash can never take down the
JVM; `Ctrl+N` opens another window with a renderer of its own, and
`Ctrl+Shift+N` opens a private one whose renderer keeps no cookies, cache or
history.

Beyond navigation it covers hover link previews and pointer shapes, zoom that
reflows the page, sharp rendering on HiDPI screens, find-in-page, drag text
selection with cut, copy and paste, a right-click menu the page can override,
camera trust prompts, downloads, dropping files onto the page, page
audio through the `nordstjernen-audio` helper, page-requested full screen,
external media playback, scrolling of overflow scrollers and the scrollbars a
page paints itself, back/forward-cache history traversal, and a JavaScript
console with page-source, layout, network and performance dumps.

```sh
# Nightly fat jar — library + browser app + icons + native libs in one file:
java -jar nordstjernen-java.jar https://example.com

# From source:
./gradlew run --args="https://example.com"
# or, after `installDist`: build/install/nordstjernen/bin/nordstjernen
```

Point it at the renderer binary with the `nordstjernen.renderer` system
property or the `NORDSTJERNEN_RENDERER` environment variable; otherwise it
looks for `nordstjernen-renderer` beside the jar, in the `builddir/src/` of
the source checkout the jar was built in, and then on `PATH`. The working
directory is never searched, so a renderer planted next to a document you
open cannot be picked up. Page audio plays when a `nordstjernen-audio` helper
sits beside the renderer.

The renderer is checked with the engine protocol handshake when it starts,
and every request to it has a deadline (two minutes, or the
`nordstjernen.timeout.ms` system property, or `RemoteBrowser.setTimeout`). A
renderer that does not answer in time is stopped and the call fails with
`RendererTimeoutException`; one that has crashed fails the next call at once
with its exit code.

### Keyboard & mouse

| Action | Keys |
| --- | --- |
| Back / Forward | `Alt+Left` / `Alt+Right`, or the mouse back/forward buttons |
| Reload | `Ctrl+R` or `F5` |
| Home | `Alt+Home` |
| Focus the URL bar | `Ctrl+L` or `Alt+D` |
| New window | `Ctrl+N` or `Ctrl+T` |
| New private window | `Ctrl+Shift+N` |
| Close the window / quit | `Ctrl+W` / `Ctrl+Q` |
| Find in page | `Ctrl+F`, then `Ctrl+G` / `Ctrl+Shift+G` |
| Zoom | `Ctrl` + `+` / `-` / `0`, or `Ctrl`+wheel |
| Cut / Copy / Paste / Select all | `Ctrl+X` / `Ctrl+C` / `Ctrl+V` / `Ctrl+A` (`Cmd` on macOS) |
| Save page as PDF | `Ctrl+P` |
| JavaScript console | `F12` or `Ctrl+Shift+J` |
| Scroll | `↑` / `↓` / `PageUp` / `PageDown` / `Space`, mouse wheel (`Shift` for sideways), or the scrollbar |
| Open a link in a new window | Middle click |
| Leave page full screen | `Esc` |

**Text input.** Click an input field, textarea or other editable element to
focus it, then type — characters, `Backspace`/`Delete`, caret movement
(`←`/`→`/`Home`/`End`), `Enter`, and `Tab` between fields are all forwarded to
the page. Key events go to whatever the page has focused, so the arrow keys
edit a focused field rather than scrolling. Paste goes to the focused field
too, and text a page copies lands on the system clipboard.

## Use as a library

### The browser API (`org.nordstjernen.browser`)

`Engine`, `BrowserContext` and `Page` are the API to build on. No engine code
is loaded into the JVM: every page runs in a `nordstjernen-renderer` process
of its own, sandboxed by the renderer itself, so a page that crashes or hangs
costs that page and nothing else.

```java
import org.nordstjernen.browser.BrowserContext;
import org.nordstjernen.browser.Engine;
import org.nordstjernen.browser.Page;
import org.nordstjernen.browser.Response;

try (Engine engine = Engine.launch();
     BrowserContext context = engine.newContext();   // a profile of its own, deleted on close
     Page page = context.newPage(1280, 800)) {

    Response response = page.navigate("https://example.com");
    response.ok();                                   // false when no document opened
    page.title();                                    // also url(), security(), serverAddress()
    page.text();                                     // the text as laid out
    page.links();                                    // absolute link URLs
    page.evaluate("document.querySelectorAll('a').length");

    byte[] png = page.screenshot();                  // the whole page, encoded by the renderer
    byte[] hi  = page.screenshot(Page.Screenshot.region(0, 0, 640, 400).withScale(2));
    byte[] pdf = page.pdf(PdfOptions.a4().margins(15)        // paged, with real text and links
        .footer(null, "Page {page} of {pages}", null));

    page.click(120, 240);                            // real pointer and click events
    page.type("hello");                              // into the focused field
    page.press("Enter");
}
```

- **Any thread.** A page serves its requests one at a time, in the order they
  arrive; nothing is tied to the thread that created it.
- **Deadlines.** Every request has one (`Engine.Options.withTimeout`,
  `Page.setTimeout`; two minutes by default). A renderer that does not answer
  is stopped and the call fails with `RendererTimeoutException`. Navigating
  the page again starts a fresh renderer.
- **Profiles.** `engine.newContext()` gives a context whose pages share
  cookies and storage with each other and with nothing else, in a directory
  that is deleted when the context closes. `ContextOptions.persistent(dir)`
  keeps a profile between runs, `ContextOptions.inMemory()` writes nothing to
  disk, and `engine.userContext()` is this user's own Nordstjernen profile.
- **PDF.** `pdf()` lays the page out again for the sheet with its print styles
  and cuts it into pages between lines, table rows and boxes that ask to stay
  whole. `PdfOptions` sets the sheet, margins, scale, page ranges, metadata
  and header and footer texts; see `docs/printing.md`.
- **No `java.desktop`.** `screenshot()` returns PNG bytes and `render()` raw
  premultiplied BGRA rows. `org.nordstjernen.swing.FrameImages` turns those
  into a `BufferedImage` for a Swing or JavaFX host.
- **A host's side of the page.** `render()` and `tick()` report what the page
  asked for (`PageEvents`): a navigation, a download (`startDownload`,
  `pollDownload`, `saveDownload`), popups (`popupOperations`,
  `deliverPopupOperation`), capability requests (`resolveCapability`), audio
  commands for `AudioHelper`, clipboard writes and fullscreen changes.

`examples/PageExample.java` is a runnable version of the snippet above.

### In-process pages over JNI (`Nordstjernen`, `Page`)

The older in-process path loads the engine into the JVM through its C
embedding API (`src/libnordstjernen.h`) and a thin JNI bridge. It fetches,
lays out, scripts and renders a page to raw pixels, a `BufferedImage`, a
PNG/PDF file or text, but has no input or script evaluation, runs one page at
a time on one thread, is not sandboxed, and a native crash takes the JVM with
it.

```java
import org.nordstjernen.Nordstjernen;
import org.nordstjernen.Page;
import java.nio.file.Path;

try (Page page = Nordstjernen.open("https://example.com", 360, 600)) {
    page.title();                                  // page <title>
    page.url();                                    // final URL (after redirects)
    page.pageSize();                               // Size[width=…, height=…] (CSS px)
    page.text();                                   // extracted (laid-out) text
    page.links();                                  // List<String> of absolute link URLs
    page.linkAt(40, 72);                           // link URL at a CSS-px point, or null

    page.renderRgba(0, 0, 720, 1280, 2.0);         // premultiplied RGBA8888 bytes
    page.render(0, 0, 720, 1280, 2.0);             // BufferedImage of a viewport
    page.renderFullPage(2.0);                      // BufferedImage of the whole page
    page.renderToFile(Path.of("example.png"));     // .pdf path → PDF, else PNG
}
Nordstjernen.shutdown();
```

`open(url, viewportWidthCss, settleMs)` lays out at `viewportWidthCss` CSS
pixels and lets scripts run for `settleMs` before the final layout. The
one-shot helpers `Nordstjernen.screenshot`, `saveScreenshot`, `textOf` and
`linksOf` open, do one thing and close. `Page` holds native memory, so always
`close()` it; `Nordstjernen.shutdown()` releases process-wide engine state.

### `RemotePage` and `RemoteBrowser`

These came before `org.nordstjernen.browser` and are deprecated.
`RemoteBrowser` now only forwards to a `Page` from that package; its numbered
`select` and `key` kinds map onto `Page.Selection` and `Page.KeyKind`.
`RemotePage` is a one-page convenience over the same renderer process.

## Download

Nightly builds publish a single runnable **fat jar** —
[`nordstjernen-java.jar`](https://www.nordstjernen.org/nightly/nordstjernen-java.jar)
— bundling the library API, the browser app, its icons, and the JNI native
libraries, alongside
[sources](https://www.nordstjernen.org/nightly/nordstjernen-java-sources.jar),
[javadoc](https://www.nordstjernen.org/nightly/nordstjernen-java-javadoc.jar)
and [API docs](https://www.nordstjernen.org/nightly/java/apidocs/). Run it with
`java -jar nordstjernen-java.jar <url>` (see the renderer note under
[Run the browser](#run-the-browser) about pointing it at a
`nordstjernen-renderer` binary).

## Use from Maven or Gradle

Releases are published to **GitHub Packages**, not yet to Maven Central. The
version is the release tag (for example `1.0.44`); the artifact for a release
holds the JNI libraries for Linux x86_64 only, and no renderer.

```xml
<repositories>
  <repository>
    <id>nordstjernen</id>
    <url>https://maven.pkg.github.com/nordstjernen-web/nordstjernen-sol-browser</url>
  </repository>
</repositories>

<dependency>
  <groupId>org.nordstjernen</groupId>
  <artifactId>nordstjernen-java</artifactId>
  <version>1.0.44</version>
</dependency>
```

```groovy
repositories {
    maven {
        url = uri('https://maven.pkg.github.com/nordstjernen-web/nordstjernen-sol-browser')
        credentials {
            username = System.getenv('GITHUB_ACTOR')
            password = System.getenv('GITHUB_TOKEN')
        }
    }
}
dependencies {
    implementation 'org.nordstjernen:nordstjernen-java:1.0.44'
}
```

GitHub Packages asks for a token even to read a public package: a personal
access token with `read:packages`, as the `nordstjernen` server in
`~/.m2/settings.xml` for Maven or as above for Gradle.

## Build

```sh
# 1. Build the engine + JNI bridge (stages libs into src/main/resources/native/<os>-<arch>/)
JAVA_HOME=/path/to/jdk-21 java/scripts/build-native.sh

# 2a. Build the jar with Gradle (bundles the native libs)
cd java && gradle build      # -> build/libs/nordstjernen-java-<version>.jar

# 2b. …or with Maven, which produces the same artifacts
cd java && mvn package       # -> target/nordstjernen-java-<version>.jar
```

`build-native.sh` runs on Linux, on macOS and, in an MSYS2 shell, on Windows.

Both builds emit the runnable jar plus `-sources` and `-javadoc`, with the
same manifest (`Main-Class`, `Automatic-Module-Name`, `Enable-Native-Access`)
and the licence texts (`License.md`, `COPYING`, `THIRD-PARTY-LICENSES.md`)
under `META-INF/`. `java/pom.xml` is the single source of truth for the
version; `build.gradle` reads it back so the two never drift.

### Publishing

```sh
mvn install                          # → ~/.m2/repository
gradle publishToMavenLocal           # the same coordinates, from Gradle
```

`.github/workflows/java-publish.yml` publishes each release to GitHub
Packages with Gradle. The POM also has a `release` profile
(`mvn -Prelease deploy`) that signs the artifacts with `maven-gpg-plugin` and
uploads them to the Maven Central portal with
`central-publishing-maven-plugin`; no workflow runs it yet, and it needs the
signing key and the `central` server credentials in `~/.m2/settings.xml`.

`scripts/build-native.sh` compiles `libnordstjernen` (via meson) and
`libnordstjernenjni` (the JNI bridge, linked with `RPATH=$ORIGIN` so it finds
the engine beside it), staging both into `src/main/resources/native/`. The
Gradle jar bundles them; `NativeLoader` unpacks and loads them at runtime.

The nightly **fat jar** (`scripts/nightly.sh`, `stage_java`) goes one step
further: it compiles every source under `src/main/java` (library **and** the
`org.nordstjernen.app` browser), folds in the toolbar icons and the staged
native libs, and writes a `Main-Class` manifest — so the one jar is both the
embedding library and the `java -jar`-launchable browser.

### Native library resolution

`NativeLoader` resolves, in order:
1. the directory in the `nordstjernen.native.dir` system property;
2. libraries bundled in the jar under `/native/<os>-<arch>/`, unpacked once
   into a per-user cache directory named after their contents
   (`nordstjernen.cache.dir` moves it);
3. the platform loader (`java.library.path` / `LD_LIBRARY_PATH`).

> The jar bundles `libnordstjernen` and the JNI bridge, but **not** the engine's
> own shared dependencies (GLib, cairo, pango, libcurl, sqlite, …). Those must
> be present on the host. Self-contained bundling of the whole stack is future
> work and tracks the Android dependency cross-build.

## CI

`.github/workflows/java.yml` builds the native libs and the jar on JDK 21, then
runs `examples/Example.java` against the **bundled** jar (exercising the
resource-extraction load path) and checks it renders `about:start`.
