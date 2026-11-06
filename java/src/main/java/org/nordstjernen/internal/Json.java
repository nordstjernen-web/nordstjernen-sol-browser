/* Nordstjernen — minimal JSON reader/writer for the renderer control protocol.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.internal;

/**
 * Just enough JSON for the flat objects the renderer speaks. Keys are located
 * by scanning for a quoted token immediately followed by {@code ':'}, so a key
 * name appearing inside a string value can never be mistaken for the key.
 */
public final class Json {

    private Json() {
    }

    public static String escape(String s) {
        if (s == null) {
            return "";
        }
        StringBuilder sb = new StringBuilder(s.length() + 8);
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"': sb.append("\\\""); break;
                case '\\': sb.append("\\\\"); break;
                case '\n': sb.append("\\n"); break;
                case '\r': sb.append("\\r"); break;
                case '\t': sb.append("\\t"); break;
                default:
                    if (c < 0x20) sb.append(String.format("\\u%04x", (int) c));
                    else sb.append(c);
            }
        }
        return sb.toString();
    }

    /** The string value of {@code key}, unescaped; {@code ""} when absent. */
    public static String string(String json, String key) {
        int i = valueStart(json, key);
        if (i < 0) {
            return "";
        }
        i = skipSpace(json, i);
        if (i >= json.length() || json.charAt(i) != '"') {
            return "";
        }
        StringBuilder sb = new StringBuilder();
        for (int j = i + 1; j < json.length(); j++) {
            char c = json.charAt(j);
            if (c == '"') {
                break;
            }
            if (c != '\\' || j + 1 >= json.length()) {
                sb.append(c);
                continue;
            }
            char nx = json.charAt(++j);
            switch (nx) {
                case 'n': sb.append('\n'); break;
                case 'r': sb.append('\r'); break;
                case 't': sb.append('\t'); break;
                case 'b': sb.append('\b'); break;
                case 'f': sb.append('\f'); break;
                case 'u':
                    if (j + 4 < json.length()) {
                        try {
                            sb.append((char) Integer.parseInt(
                                json.substring(j + 1, j + 5), 16));
                        } catch (NumberFormatException ignored) {
                            // leave the malformed escape out
                        }
                        j += 4;
                    }
                    break;
                default: sb.append(nx);
            }
        }
        return sb.toString();
    }

    /** The string value of {@code key}, or {@code null} when absent or empty. */
    public static String stringOrNull(String json, String key) {
        String s = string(json, key);
        return s.isEmpty() ? null : s;
    }

    /** The integer value of {@code key}, or {@code fallback} when absent. */
    public static int integer(String json, String key, int fallback) {
        int i = valueStart(json, key);
        if (i < 0) {
            return fallback;
        }
        i = skipSpace(json, i);
        int start = i;
        if (i < json.length() && (json.charAt(i) == '-' || json.charAt(i) == '+')) {
            i++;
        }
        int digits = i;
        while (i < json.length() && Character.isDigit(json.charAt(i))) {
            i++;
        }
        if (i == digits) {
            return fallback;
        }
        try {
            return Integer.parseInt(json.substring(start, i));
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    /** The integer value of {@code key} as a long, or {@code fallback} when absent. */
    public static long longInteger(String json, String key, long fallback) {
        int i = valueStart(json, key);
        if (i < 0) {
            return fallback;
        }
        i = skipSpace(json, i);
        int start = i;
        if (i < json.length() && (json.charAt(i) == '-' || json.charAt(i) == '+')) {
            i++;
        }
        int digits = i;
        while (i < json.length() && Character.isDigit(json.charAt(i))) {
            i++;
        }
        if (i == digits) {
            return fallback;
        }
        try {
            return Long.parseLong(json.substring(start, i));
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    /** True when {@code key} holds a non-zero integer. */
    public static boolean flag(String json, String key) {
        return integer(json, key, 0) != 0;
    }

    private static int skipSpace(String json, int i) {
        while (i < json.length() && Character.isWhitespace(json.charAt(i))) {
            i++;
        }
        return i;
    }

    private static int valueStart(String json, String key) {
        if (json == null) {
            return -1;
        }
        int n = json.length();
        int i = 0;
        int tokenStart = -1;
        boolean inString = false;
        while (i < n) {
            char c = json.charAt(i);
            if (!inString) {
                if (c == '"') {
                    inString = true;
                    tokenStart = i;
                }
                i++;
                continue;
            }
            if (c == '\\') {
                i += 2;
                continue;
            }
            if (c != '"') {
                i++;
                continue;
            }
            inString = false;
            int j = skipSpace(json, i + 1);
            if (j < n && json.charAt(j) == ':'
                && matches(json, tokenStart + 1, i, key)) {
                return j + 1;
            }
            i++;
        }
        return -1;
    }

    private static boolean matches(String json, int from, int to, String key) {
        if (to - from != key.length()) {
            return false;
        }
        return json.regionMatches(from, key, 0, key.length());
    }

    /** {@code s} as a JSON string literal, quotes included. */
    public static String quote(String s) {
        return "\"" + escape(s) + "\"";
    }

    /**
     * The JSON text of a value: a {@code Map} with string keys, an
     * {@code Iterable}, a {@code String}, a {@code Number}, a {@code Boolean}
     * or null; anything else is written as its {@code toString()}.
     */
    public static String write(Object value) {
        StringBuilder out = new StringBuilder();
        write(out, value);
        return out.toString();
    }

    private static void write(StringBuilder out, Object value) {
        if (value == null || value instanceof Boolean) {
            out.append(value);
        } else if (value instanceof Number number) {
            double d = number.doubleValue();
            out.append(Double.isFinite(d) ? number.toString() : "null");
        } else if (value instanceof java.util.Map<?, ?> map) {
            String separator = "{";
            for (java.util.Map.Entry<?, ?> entry : map.entrySet()) {
                out.append(separator).append(quote(String.valueOf(entry.getKey()))).append(':');
                write(out, entry.getValue());
                separator = ",";
            }
            out.append(map.isEmpty() ? "{}" : "}");
        } else if (value instanceof Iterable<?> items) {
            String separator = "[";
            for (Object item : items) {
                out.append(separator);
                write(out, item);
                separator = ",";
            }
            out.append(separator.equals("[") ? "[]" : "]");
        } else {
            out.append(quote(value.toString()));
        }
    }

    /**
     * Parse a whole JSON text. Objects become {@code Map<String, Object>} in
     * document order, arrays {@code List<Object>}, strings {@code String},
     * numbers {@code Long} when integral and {@code Double} otherwise,
     * {@code true}/{@code false} {@code Boolean}, and {@code null} null.
     *
     * @throws IllegalArgumentException when the text is not valid JSON
     */
    public static Object parse(String text) {
        Parser parser = new Parser(text == null ? "" : text);
        parser.skipSpace();
        Object value = parser.value(0);
        parser.skipSpace();
        if (parser.at < parser.text.length()) {
            throw parser.error("unexpected text after the value");
        }
        return value;
    }

    private static final class Parser {
        private static final int MAX_DEPTH = 256;

        final String text;
        int at;

        Parser(String text) {
            this.text = text;
        }

        IllegalArgumentException error(String what) {
            return new IllegalArgumentException("JSON: " + what + " at offset " + at);
        }

        void skipSpace() {
            while (at < text.length() && " \t\r\n".indexOf(text.charAt(at)) >= 0) {
                at++;
            }
        }

        Object value(int depth) {
            if (depth > MAX_DEPTH) {
                throw error("nesting too deep");
            }
            if (at >= text.length()) {
                throw error("unexpected end");
            }
            char c = text.charAt(at);
            switch (c) {
                case '{': return object(depth);
                case '[': return array(depth);
                case '"': return string();
                case 't': return literal("true", Boolean.TRUE);
                case 'f': return literal("false", Boolean.FALSE);
                case 'n': return literal("null", null);
                default: return number();
            }
        }

        private Object literal(String word, Object value) {
            if (!text.startsWith(word, at)) {
                throw error("unexpected token");
            }
            at += word.length();
            return value;
        }

        private java.util.Map<String, Object> object(int depth) {
            java.util.Map<String, Object> out = new java.util.LinkedHashMap<>();
            at++;
            skipSpace();
            if (at < text.length() && text.charAt(at) == '}') {
                at++;
                return out;
            }
            while (true) {
                skipSpace();
                if (at >= text.length() || text.charAt(at) != '"') {
                    throw error("expected a key");
                }
                String key = string();
                skipSpace();
                if (at >= text.length() || text.charAt(at) != ':') {
                    throw error("expected ':'");
                }
                at++;
                skipSpace();
                out.put(key, value(depth + 1));
                skipSpace();
                if (at < text.length() && text.charAt(at) == ',') {
                    at++;
                    continue;
                }
                if (at < text.length() && text.charAt(at) == '}') {
                    at++;
                    return out;
                }
                throw error("expected ',' or '}'");
            }
        }

        private java.util.List<Object> array(int depth) {
            java.util.List<Object> out = new java.util.ArrayList<>();
            at++;
            skipSpace();
            if (at < text.length() && text.charAt(at) == ']') {
                at++;
                return out;
            }
            while (true) {
                skipSpace();
                out.add(value(depth + 1));
                skipSpace();
                if (at < text.length() && text.charAt(at) == ',') {
                    at++;
                    continue;
                }
                if (at < text.length() && text.charAt(at) == ']') {
                    at++;
                    return out;
                }
                throw error("expected ',' or ']'");
            }
        }

        private String string() {
            StringBuilder sb = new StringBuilder();
            at++;
            while (at < text.length()) {
                char c = text.charAt(at++);
                if (c == '"') {
                    return sb.toString();
                }
                if (c != '\\') {
                    sb.append(c);
                    continue;
                }
                if (at >= text.length()) {
                    break;
                }
                char escaped = text.charAt(at++);
                switch (escaped) {
                    case 'n': sb.append('\n'); break;
                    case 'r': sb.append('\r'); break;
                    case 't': sb.append('\t'); break;
                    case 'b': sb.append('\b'); break;
                    case 'f': sb.append('\f'); break;
                    case 'u':
                        if (at + 4 > text.length()) {
                            throw error("short \\u escape");
                        }
                        try {
                            sb.append((char) Integer.parseInt(text.substring(at, at + 4), 16));
                        } catch (NumberFormatException e) {
                            throw error("bad \\u escape");
                        }
                        at += 4;
                        break;
                    default: sb.append(escaped);
                }
            }
            throw error("unterminated string");
        }

        private Object number() {
            int start = at;
            while (at < text.length() && "+-0123456789.eE".indexOf(text.charAt(at)) >= 0) {
                at++;
            }
            String token = text.substring(start, at);
            if (token.isEmpty()) {
                throw error("unexpected character");
            }
            try {
                if (token.indexOf('.') < 0 && token.indexOf('e') < 0 && token.indexOf('E') < 0) {
                    return Long.parseLong(token);
                }
                return Double.parseDouble(token);
            } catch (NumberFormatException e) {
                at = start;
                throw error("bad number");
            }
        }
    }
}
