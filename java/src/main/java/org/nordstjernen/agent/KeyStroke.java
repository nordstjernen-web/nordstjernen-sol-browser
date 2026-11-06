/* Nordstjernen — a key or shortcut named in text, as key events.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.agent;

import java.util.Locale;
import java.util.Map;

/**
 * One key press parsed from text such as {@code Enter}, {@code ctrl+a} or
 * {@code shift+Tab}.
 *
 * @param key       the {@code KeyboardEvent.key} value
 * @param code      the {@code KeyboardEvent.code} value
 * @param keyCode   the legacy numeric key code
 * @param modifiers shift 1, control 2, alt 4, meta 8
 */
record KeyStroke(String key, String code, int keyCode, int modifiers) {

    static final int SHIFT = 1;
    static final int CONTROL = 2;
    static final int ALT = 4;
    static final int META = 8;

    private static final Map<String, Integer> NAMED = Map.ofEntries(
        Map.entry("Backspace", 8), Map.entry("Tab", 9), Map.entry("Enter", 13),
        Map.entry("Escape", 27), Map.entry("PageUp", 33), Map.entry("PageDown", 34),
        Map.entry("End", 35), Map.entry("Home", 36), Map.entry("ArrowLeft", 37),
        Map.entry("ArrowUp", 38), Map.entry("ArrowRight", 39), Map.entry("ArrowDown", 40),
        Map.entry("Insert", 45), Map.entry("Delete", 46));

    private static final Map<String, String> ALIASES = Map.ofEntries(
        Map.entry("return", "Enter"), Map.entry("esc", "Escape"), Map.entry("del", "Delete"),
        Map.entry("up", "ArrowUp"), Map.entry("down", "ArrowDown"),
        Map.entry("left", "ArrowLeft"), Map.entry("right", "ArrowRight"),
        Map.entry("pgup", "PageUp"), Map.entry("pgdn", "PageDown"),
        Map.entry("spacebar", "Space"));

    /**
     * Parse one press: key names as in {@code KeyboardEvent.key}, a single
     * character, {@code Space} or {@code F1} to {@code F12}, with any of
     * {@code shift}, {@code ctrl}, {@code alt} and {@code meta} joined by
     * {@code +} in front.
     */
    static KeyStroke parse(String text) {
        String[] parts = text.trim().split("\\+(?=.)");
        int modifiers = 0;
        for (int i = 0; i < parts.length - 1; i++) {
            modifiers |= modifier(parts[i]);
        }
        return named(parts[parts.length - 1], modifiers);
    }

    private static int modifier(String name) {
        return switch (name.trim().toLowerCase(Locale.ROOT)) {
            case "shift" -> SHIFT;
            case "ctrl", "control" -> CONTROL;
            case "alt", "option" -> ALT;
            case "meta", "cmd", "command", "super", "win" -> META;
            default -> throw new IllegalArgumentException("unknown modifier: " + name);
        };
    }

    private static KeyStroke named(String name, int modifiers) {
        String wanted = ALIASES.getOrDefault(name.toLowerCase(Locale.ROOT), name);
        if (wanted.equalsIgnoreCase("Space") || wanted.equals(" ")) {
            return new KeyStroke(" ", "Space", 32, modifiers);
        }
        for (Map.Entry<String, Integer> entry : NAMED.entrySet()) {
            if (entry.getKey().equalsIgnoreCase(wanted)) {
                return new KeyStroke(entry.getKey(), entry.getKey(), entry.getValue(), modifiers);
            }
        }
        if (wanted.matches("[Ff]([1-9]|1[0-2])")) {
            int n = Integer.parseInt(wanted.substring(1));
            return new KeyStroke("F" + n, "F" + n, 111 + n, modifiers);
        }
        if (wanted.codePointCount(0, wanted.length()) != 1) {
            throw new IllegalArgumentException("unknown key: " + name);
        }
        char upper = Character.toUpperCase(wanted.charAt(0));
        String code = upper >= 'A' && upper <= 'Z' ? "Key" + upper
            : upper >= '0' && upper <= '9' ? "Digit" + upper : "";
        String key = (modifiers & SHIFT) != 0 ? wanted.toUpperCase(Locale.ROOT) : wanted;
        return new KeyStroke(key, code, code.isEmpty() ? 0 : upper, modifiers);
    }

    /** Whether pressing this types its key into a text field. */
    boolean typesText() {
        return key.codePointCount(0, key.length()) == 1
            && (modifiers & (CONTROL | ALT | META)) == 0;
    }

    boolean isShortcut(char letter) {
        return (modifiers & (CONTROL | META)) != 0 && (modifiers & ALT) == 0
            && key.equalsIgnoreCase(String.valueOf(letter));
    }
}
