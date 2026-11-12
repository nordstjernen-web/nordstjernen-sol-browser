//! Nordstjernen — the document's quirks mode from its DOCTYPE, as the "initial" insertion mode decides it.
//! Copyright 2026 Andreas Røsdal
//! SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later

use crate::QuirksMode;
use crate::tokenizer::Doctype;

const QUIRKS_PREFIXES: [&[u8]; 55] = [
    b"+//silmaril//dtd html pro v0r11 19970101//",
    b"-//as//dtd html 3.0 aswedit + extensions//",
    b"-//advasoft ltd//dtd html 3.0 aswedit + extensions//",
    b"-//ietf//dtd html 2.0 level 1//",
    b"-//ietf//dtd html 2.0 level 2//",
    b"-//ietf//dtd html 2.0 strict level 1//",
    b"-//ietf//dtd html 2.0 strict level 2//",
    b"-//ietf//dtd html 2.0 strict//",
    b"-//ietf//dtd html 2.0//",
    b"-//ietf//dtd html 2.1e//",
    b"-//ietf//dtd html 3.0//",
    b"-//ietf//dtd html 3.2 final//",
    b"-//ietf//dtd html 3.2//",
    b"-//ietf//dtd html 3//",
    b"-//ietf//dtd html level 0//",
    b"-//ietf//dtd html level 1//",
    b"-//ietf//dtd html level 2//",
    b"-//ietf//dtd html level 3//",
    b"-//ietf//dtd html strict level 0//",
    b"-//ietf//dtd html strict level 1//",
    b"-//ietf//dtd html strict level 2//",
    b"-//ietf//dtd html strict level 3//",
    b"-//ietf//dtd html strict//",
    b"-//ietf//dtd html//",
    b"-//metrius//dtd metrius presentational//",
    b"-//microsoft//dtd internet explorer 2.0 html strict//",
    b"-//microsoft//dtd internet explorer 2.0 html//",
    b"-//microsoft//dtd internet explorer 2.0 tables//",
    b"-//microsoft//dtd internet explorer 3.0 html strict//",
    b"-//microsoft//dtd internet explorer 3.0 html//",
    b"-//microsoft//dtd internet explorer 3.0 tables//",
    b"-//netscape comm. corp.//dtd html//",
    b"-//netscape comm. corp.//dtd strict html//",
    b"-//o'reilly and associates//dtd html 2.0//",
    b"-//o'reilly and associates//dtd html extended 1.0//",
    b"-//o'reilly and associates//dtd html extended relaxed 1.0//",
    b"-//sq//dtd html 2.0 hotmetal + extensions//",
    b"-//softquad software//dtd hotmetal pro 6.0::19990601::extensions to html 4.0//",
    b"-//softquad//dtd hotmetal pro 4.0::19971010::extensions to html 4.0//",
    b"-//spyglass//dtd html 2.0 extended//",
    b"-//sun microsystems corp.//dtd hotjava html//",
    b"-//sun microsystems corp.//dtd hotjava strict html//",
    b"-//w3c//dtd html 3 1995-03-24//",
    b"-//w3c//dtd html 3.2 draft//",
    b"-//w3c//dtd html 3.2 final//",
    b"-//w3c//dtd html 3.2//",
    b"-//w3c//dtd html 3.2s draft//",
    b"-//w3c//dtd html 4.0 frameset//",
    b"-//w3c//dtd html 4.0 transitional//",
    b"-//w3c//dtd html experimental 19960712//",
    b"-//w3c//dtd html experimental 970421//",
    b"-//w3c//dtd w3 html//",
    b"-//w3o//dtd w3 html 3.0//",
    b"-//webtechs//dtd mozilla html 2.0//",
    b"-//webtechs//dtd mozilla html//",
];

const QUIRKS_EXACT: [&[u8]; 3] = [
    b"-//w3o//dtd w3 html strict 3.0//en//",
    b"-/w3c/dtd html 4.0 transitional/en",
    b"html",
];

const HTML401: [&[u8]; 2] = [
    b"-//w3c//dtd html 4.01 frameset//",
    b"-//w3c//dtd html 4.01 transitional//",
];

const XHTML10: [&[u8]; 2] = [
    b"-//w3c//dtd xhtml 1.0 frameset//",
    b"-//w3c//dtd xhtml 1.0 transitional//",
];

fn starts_with_any(value: &[u8], prefixes: &[&[u8]]) -> bool {
    prefixes.iter().any(|p| value.starts_with(p))
}

pub fn quirks_mode(doctype: &Doctype) -> QuirksMode {
    let public = doctype.public_id.as_deref().map(<[u8]>::to_ascii_lowercase);
    let system = doctype.system_id.as_deref().map(<[u8]>::to_ascii_lowercase);
    let public_ref = public.as_deref().unwrap_or(b"");
    if doctype.force_quirks
        || doctype.name.as_deref() != Some(b"html")
        || QUIRKS_EXACT.contains(&public_ref)
        || system.as_deref() == Some(b"http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd")
        || starts_with_any(public_ref, &QUIRKS_PREFIXES)
        || (system.is_none() && starts_with_any(public_ref, &HTML401))
    {
        return QuirksMode::Quirks;
    }
    if starts_with_any(public_ref, &XHTML10)
        || (system.is_some() && starts_with_any(public_ref, &HTML401))
    {
        return QuirksMode::LimitedQuirks;
    }
    QuirksMode::NoQuirks
}
