/* Nordstjernen browser API — runnable example / smoke test.
 *
 * Compile against the library jar and run with a renderer the library can
 * find (NORDSTJERNEN_RENDERER, -Dnordstjernen.renderer=…, beside the jar, or
 * on PATH):
 *   javac -cp nordstjernen-java.jar examples/PageExample.java -d out
 *   java -cp nordstjernen-java.jar:out PageExample https://example.com
 */

import org.nordstjernen.browser.BrowserContext;
import org.nordstjernen.browser.Engine;
import org.nordstjernen.browser.Page;
import org.nordstjernen.browser.Response;

import java.nio.file.Files;
import java.nio.file.Path;

public final class PageExample {

    public static void main(String[] args) throws Exception {
        String url = args.length > 0 ? args[0] : "about:start";
        try (Engine engine = Engine.launch();
             BrowserContext context = engine.newContext();
             Page page = context.newPage(1000, 700)) {
            Response response = page.navigate(url);
            System.out.println("ok:     " + response.ok());
            System.out.println("engine: " + page.engineVersion());
            System.out.println("title:  " + page.title());
            System.out.println("url:    " + page.url());
            System.out.println("size:   " + page.pageWidth() + "x" + page.pageHeight() + " css px");

            String text = page.text().replace('\n', ' ').strip();
            System.out.println("text:   " + text.substring(0, Math.min(160, text.length())));
            System.out.println("links:  " + page.links().size() + " found");
            System.out.println("script: 6 * 7 = " + page.evaluate("6 * 7"));

            byte[] png = page.screenshot();
            Path out = Path.of("page.png");
            Files.write(out, png);
            System.out.println("wrote:  " + out.toAbsolutePath() + " (" + png.length + " bytes)");
        }
    }
}
