/* Nordstjernen — what a page asked of its host since the last frame.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.browser;

import java.util.List;

/**
 * Requests a page raised while it rendered or ticked. The renderer never
 * calls its host; these ride back on every {@link Page#render} and
 * {@link Page#tick}, and each is reported once.
 *
 * @param navigation     a URL the page wants loaded (a followed link, a
 *                       script navigation), or null
 * @param downloadUrl    a download the page offered, or null; fetch it with
 *                       {@link Page#startDownload}
 * @param downloadName   the file name the page suggested for it, or null
 * @param windowAction   {@code fullscreen-enter} or {@code fullscreen-exit},
 *                       or null
 * @param audio          commands for the {@code nordstjernen-audio} helper
 * @param clipboardWrite the page wrote to the clipboard; read the text with
 *                       {@link Page#clipboardText}
 * @param popups         popup operations are queued; fetch them with
 *                       {@link Page#popupOperations}
 * @param capabilities   capability requests waiting for a decision
 * @param webglOrigin    an origin that started using WebGL, or null
 */
public record PageEvents(String navigation, String downloadUrl, String downloadName,
                         String windowAction, List<String> audio, boolean clipboardWrite,
                         boolean popups, List<CapabilityRequest> capabilities,
                         String webglOrigin) {

    /** Whether anything at all was raised. */
    public boolean isEmpty() {
        return navigation == null && downloadUrl == null && windowAction == null
            && audio.isEmpty() && !clipboardWrite && !popups && capabilities.isEmpty()
            && webglOrigin == null;
    }
}
