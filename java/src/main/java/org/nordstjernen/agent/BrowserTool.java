/* Nordstjernen — one browser action offered to a model as a tool.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.agent;

import java.util.Map;

/**
 * A browser action in the shape tool-calling models and their frameworks
 * expect: a name, a description to put in front of the model, a JSON Schema
 * for the arguments and a function to run.
 *
 * @param name        the tool's name, such as {@code click}
 * @param title       a short human-readable name
 * @param description what the tool does and when to use it, for the model
 * @param inputSchema the JSON Schema of the arguments object, as JSON text
 * @param handler     the action itself
 */
public record BrowserTool(String name, String title, String description, String inputSchema,
                          Handler handler) {

    /** What a tool call came to. */
    public record Result(String text, byte[] png, boolean error) {

        public static Result text(String text) {
            return new Result(text, null, false);
        }

        public static Result image(String text, byte[] png) {
            return new Result(text, png, false);
        }

        public static Result error(String message) {
            return new Result(message, null, true);
        }
    }

    /** Runs a tool with the arguments a model gave, parsed from JSON. */
    @FunctionalInterface
    public interface Handler {
        Result call(Map<String, Object> arguments);
    }
}
