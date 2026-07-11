use serde_json::{json, Value};
use std::sync::mpsc::Sender;
use std::time::Instant;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Decision {
    Wait,
    /// Execute the exact circled canonical range in the local Python REPL.
    Run {
        consume: usize,
        start: usize,
        end: usize,
    },
    /// Route the exact circled source through rat's runtime resolver.
    Rat {
        consume: usize,
        start: usize,
        end: usize,
        selector: String,
    },
    /// Convert the circled table to a persistent Python or R data frame.
    Data {
        consume: usize,
        start: usize,
        end: usize,
        runtime: String,
        name: String,
        json: String,
    },
    /// Handwriting recognized as both display LaTeX and executable SymPy.
    Math {
        consume: usize,
        start: usize,
        end: usize,
        latex: String,
        semantic: String,
    },
    /// Evaluate the locally selected canonical math block.
    MathCalculate {
        consume: usize,
        start: usize,
        end: usize,
    },
    Edit {
        consume: usize,
        start: usize,
        end: usize,
        text: String,
    },
    /// Atomic exact-text replacements produced from free-form paper markup.
    PaperEdits {
        edits: Vec<(String, String)>,
    },
}

#[derive(Clone)]
pub struct Oracle {
    agent: ureq::Agent,
    base: String,
    key: String,
    model: String,
}

impl Oracle {
    pub fn from_env() -> Result<Self, String> {
        let key = std::env::var("INKTYPE_OPENAI_KEY")
            .or_else(|_| std::env::var("RIDDLE_OPENAI_KEY"))
            .map_err(|_| "set INKTYPE_OPENAI_KEY (or RIDDLE_OPENAI_KEY)".to_string())?;
        let base = std::env::var("INKTYPE_OPENAI_BASE")
            .or_else(|_| std::env::var("RIDDLE_OPENAI_BASE"))
            .unwrap_or_else(|_| "https://generativelanguage.googleapis.com/v1beta/openai".into())
            .trim_end_matches('/')
            .to_string();
        // Deliberately do not inherit Riddle's model: InkType needs the
        // lowest-latency vision model even when it reuses Riddle's key/base.
        let model = std::env::var("INKTYPE_OPENAI_MODEL")
            .unwrap_or_else(|_| "gemini-3.1-flash-lite".into());
        let agent = ureq::AgentBuilder::new()
            .timeout_connect(std::time::Duration::from_secs(5))
            .timeout_read(std::time::Duration::from_secs(15))
            .build();
        eprintln!("inktype: oracle base={base} model={model} reasoning=none max_tokens=128");
        Ok(Self {
            agent,
            base,
            key,
            model,
        })
    }

    pub fn ask(&self, req: Request, tx: Sender<Reply>) {
        let this = self.clone();
        std::thread::spawn(move || {
            let t0 = Instant::now();
            let result = this.ask_sync(&req);
            eprintln!(
                "inktype: request={} force={} pending={} crop={}x{} model_ms={} result={:?}",
                req.id,
                req.force,
                req.pending_count,
                req.crop_w,
                req.crop_h,
                t0.elapsed().as_millis(),
                result
            );
            let _ = tx.send((req.id, req.input_revision, req.text_revision, result));
        });
    }

    fn ask_sync(&self, req: &Request) -> Result<Decision, String> {
        let system = if req.paper_edit {
            "You are a precise paper-edit interpreter. The image is a complete annotated paper document. TEXT is the authoritative canonical text; dark pen marks are editing instructions, never document content. Interpret circles, arrows, strike-throughs, carets, insertion text, move/reorder marks, tab symbols, and notes such as remove tab, indent, dedent, move to new function, or name it x_func. Return exactly PATCH:[{\"old_text\":\"exact source\",\"new_text\":\"replacement\"}] on one line. The value immediately after PATCH: must be a compact JSON array; never output the literal word JSON. Each old_text must be an exact, nonempty, uniquely occurring substring of TEXT. Use one replacement per independent region; include enough unchanged context to make old_text unique. Preserve significant Python whitespace and produce runnable Python when editing Python. For a move, remove the source and insert it at the destination using non-overlapping replacements. Never alter unmarked text. Never return offsets, prose, Markdown, or handwritten annotation text. Return ? if the markup is ambiguous."
        } else if req.command {
            "You are classifying a handwriting COMMAND, never transcribing ordinary document text. The first unresolved gesture is a loose circle or a straight right-margin selection line around/beside existing canonical TYPESET text. Every other unresolved gesture belongs only to the handwritten action. HINTS supplies the authoritative selected range and exact SELECTED text. Never insert the action word itself, never delete SELECTED merely because the action is unclear, and never treat selection marks as text or erasers. Recognize one action: sort, calculate, correct (including the handwritten alias fix), reflow, or a rat runtime selector. If the selected text is an [[INKTYPE_MATH:...]] block and the action is calculate, return exactly MATHCALC:N,S,E using decimal integers. For correct/fix, EDIT means replace exactly the authoritative selected range with one corrected copy; never append the correction, duplicate a line, or return an insertion range. Preserve the language, exact significant indentation, and complete opening/closing backtick fences. When the selection is Python, return syntactically runnable Python: preserve block indentation, dedent only when required by its surrounding source, and never replace spaces with prose formatting. For reflow, join selected lines as one paragraph. A rat runtime selector executes the selection. Handwritten 'run' is the selector 'py'. Handwritten 'py', 'r', 'pi', or a named selector such as 'my-py-here' executes the exact selection through rat. For runtime execution return exactly RAT:N,S,E:SELECTOR with all pending gestures consumed and exact selected offsets, substituting decimal integers for N, S, and E (never output the letter N); do not interpret, rewrite, or execute the source yourself. A handwritten 'df NAME', 'py df NAME', or 'r df NAME' converts the selected table to a persistent data frame (default runtime py and default name df). For this return exactly DATA:N,S,E:RUNTIME:NAME:JSON using decimal integers (never the letter N), where JSON is one compact object {\"columns\":[strings],\"rows\":[arrays]}; infer numbers, booleans and null, preserve strings literally, and never add rows or columns. For transformations return exactly EDIT:N:S:E:T on one line, where N consumes every pending gesture, S and E are the exact selected offsets from HINTS, and T is only the corrected/transformed SELECTED text. Example: selected range 6..17 is 'my hands is', action correct, 9 gestures pending -> EDIT:9:6:17:my name is. Never output the action word, the whole document, a before/after explanation, or multiple lines outside T. No prose, markdown, or quotes. Encode structural line breaks in T as the literal token <NL> (never use \\n, because Python code may contain that escape). If the circle has no legible action beside it yet, return ?."
        } else if req.math_mode {
            "You are a handwritten-mathematics transcriber. The darker unresolved handwriting is exactly one mathematical expression. Return exactly MATH:N,S,E:JSON, where N consumes every pending gesture, S and E are the supplied ANCHOR, and JSON is one compact object with string fields latex (display LaTeX without dollar delimiters) and semantic (executable SymPy syntax). Examples: {\"latex\":\"\\\\frac{1}{2}\",\"semantic\":\"Rational(1,2)\"}; {\"latex\":\"\\\\int_0^1 x^2\\\\,dx\",\"semantic\":\"Integral(x**2,(x,0,1))\"}. Do not return the ordinary edit protocol, prose, markdown, or incomplete trailing operators. Return ? if the expression is visibly unfinished."
        } else {
            "You are a literal, low-latency handwriting and code reconciler, not an autocomplete system. The image shows ruled paper, canonical TYPESET text, and darker unresolved HANDWRITING. Transcribe programming text character-for-character: preserve leading spaces on every line, punctuation, underscores, quotes, parentheses, colons, and visible line breaks. For Python, indentation is syntax; never flatten, reflow, auto-correct, or invent code, and ensure the transcription retains the indentation shown on the ruled lines. TEXT is the exact canonical document. This is TEXT MODE: transcribe every gesture as ordinary text, even if it resembles a letter used in mathematics. Never return MATH or JSON. COMMAND FAIL-SAFE: if the first unresolved gesture is a large loop around existing typeset text and later strokes look like an action such as correct, sort, calculate, reflow, run, py, or r, return ? and never delete or replace the enclosed text. Ignore the top-right 'AI' status, mode button, and any thin vertical caret bar. Incorporate only letters actually handwritten; never invent likely words. Preserve visible word boundaries: a clear horizontal gap between words must become one space, and a GAP_AFTER_TEXT hint means T should normally begin with a space. Recently typeset tail characters are provisional: when new adjacent ink makes an earlier recognition demonstrably wrong, include those recent character offsets in [S,E) and correct them instead of merely appending. Consume only the oldest confidently resolved prefix of PENDING; leave an ambiguous trailing glyph pending. Reply with EXACTLY one line, no prose and no markdown. '?' means no confident prefix can yet be resolved. Otherwise output N,S,E:T: consume N oldest pending gestures and replace character range [S,E) with raw text T. Never quote T. Empty T deletes. All indices are Unicode character offsets and must satisfy 0<=S<=E<=LEN. LINES maps each typeset line's screen baseline y to its character offsets; use it with the gesture coordinates to pick S and E precisely between the right characters. Writing on a lower ruled line than the last typeset line means the user started a new line: put <NL> in T where lines break (one per blank ruled line skipped). Handwriting that itself spans several ruled lines joins with <NL>. If a CARET hint is given, the user erased text there and is writing its replacement: insert at that offset. Prefer consuming all PENDING together; if their trailing glyph is incomplete return ?. Examples: TEXT='' with three gestures spelling Hi -> 3,0,0:Hi ; TEXT='hel' with handwriting lo -> 2,3,3:lo ; TEXT='cat' (LEN=3) with handwriting dog on the next ruled line -> 3,3,3:<NL>dog ; TEXT='a big cat' CARET=2 with handwriting red -> 3,2,2:red."
        };
        let force = if req.force {
            "This is a forced retry after a pause: decide unless genuinely illegible."
        } else {
            "Return ? only if one more pen stroke is clearly needed."
        };
        let hint = if req.hint.is_empty() {
            String::new()
        } else {
            format!("HINTS: {}\n", req.hint)
        };
        let prompt = format!(
            "TEXT={:?}\nLEN={} ANCHOR={}\nLINES:\n{}\nPENDING={} oldest-first; tool={}\nGESTURES={} (p=pen; screen endpoints)\nCROP screen=({},{}), size={}x{}\n{}{}",
            req.text, req.text.chars().count(), req.anchor, req.line_map, req.pending_count, req.tool, req.gesture_summary,
            req.crop_x, req.crop_y, req.crop_w, req.crop_h, hint, force
        );
        let dots_text = self.model.contains("dots.ocr")
            && !req.command
            && !req.math_mode
            && !req.paper_edit;
        let system = if dots_text {
            "Return only the literal handwritten text visible in the image. Preserve every character, leading indentation, spaces, and line break. For Python code, indentation and punctuation must remain exactly runnable as written. Do not explain, reflow, auto-correct, or add Markdown fences."
        } else {
            system
        };
        let prompt = if dots_text {
            "Extract the text content from this image.".to_string()
        } else {
            prompt
        };
        let slot = req.id % 20;
        debug_write(slot, "png", &req.png);
        debug_write(
            slot,
            "request.txt",
            format!(
                "id={} input_revision={} text_revision={} force={} model={}\nSYSTEM={}\n{}\n",
                req.id,
                req.input_revision,
                req.text_revision,
                req.force,
                self.model,
                system,
                prompt
            )
            .as_bytes(),
        );
        let mut body = json!({
            "model": self.model,
            "stream": false,
            "temperature": 0,
            "reasoning_effort": "none",
            "max_tokens": if req.paper_edit { 2048 } else if req.command { 512 } else { 128 },
            "messages": [
                {"role":"system", "content":system},
                {"role":"user", "content":[
                    {"type":"text", "text":prompt},
                    {"type":"image_url", "image_url":{"url":format!("data:image/png;base64,{}", base64(&req.png))}}
                ]}
            ]
        });
        // vLLM does not translate OpenAI's `reasoning_effort=none` into
        // Qwen's chat-template switch. Without this, the model spends the
        // entire 128-token budget on hidden reasoning and returns null content.
        if !self.base.contains("api.groq.com") {
            body.as_object_mut().unwrap().insert(
                "chat_template_kwargs".into(),
                json!({"enable_thinking": false}),
            );
        }
        let response = self
            .agent
            .post(&format!("{}/chat/completions", self.base))
            .set("Authorization", &format!("Bearer {}", self.key))
            .set("Content-Type", "application/json")
            .send_json(body)
            .map_err(|e| match e {
                ureq::Error::Status(code, r) => {
                    let detail = r.into_string().unwrap_or_default();
                    debug_write(slot, "response.json", detail.as_bytes());
                    format!("http {code}: {detail}")
                }
                other => format!("request: {other}"),
            })?;
        let response_text = response
            .into_string()
            .map_err(|e| format!("read response: {e}"))?;
        debug_write(slot, "response.json", response_text.as_bytes());
        let value: Value =
            serde_json::from_str(&response_text).map_err(|e| format!("response json: {e}"))?;
        let input_tokens = value
            .pointer("/usage/prompt_tokens")
            .and_then(Value::as_u64)
            .unwrap_or(0);
        let output_tokens = value
            .pointer("/usage/completion_tokens")
            .and_then(Value::as_u64)
            .unwrap_or(0);
        if self.base.contains("api.groq.com") {
            // Current qwen/qwen3.6-27b Groq rates: $0.60/M input and $3/M output.
            let estimated_usd = input_tokens as f64 * 0.60 / 1_000_000.0
                + output_tokens as f64 * 3.00 / 1_000_000.0;
            eprintln!(
                "inktype: request={} usage input_tokens={} output_tokens={} estimated_usd={:.6}",
                req.id, input_tokens, output_tokens, estimated_usd
            );
        } else {
            eprintln!(
                "inktype: request={} usage input_tokens={} output_tokens={} local_cost_usd=0",
                req.id, input_tokens, output_tokens
            );
        }
        let raw = value
            .pointer("/choices/0/message/content")
            .and_then(Value::as_str)
            .ok_or_else(|| format!("no content: {value}"))?;
        if dots_text {
            let transcription = raw.trim();
            if transcription.is_empty() || transcription == "?" {
                return Ok(Decision::Wait);
            }
            return Ok(Decision::Edit {
                consume: req.pending_count,
                start: req.anchor,
                end: req.anchor,
                text: transcription.to_string(),
            });
        }
        parse_decision(raw, req.pending_count, req.text.chars().count(), req.anchor)
            .map_err(|e| format!("{e}; raw={raw:?}"))
    }
}

pub struct Request {
    pub id: u64,
    pub input_revision: u64,
    pub text_revision: u64,
    pub text: String,
    pub anchor: usize,
    pub pending_count: usize,
    pub gesture_summary: String,
    pub tool: &'static str,
    pub line_map: String,
    pub hint: String,
    pub command: bool,
    pub math_mode: bool,
    pub paper_edit: bool,
    pub crop_x: i32,
    pub crop_y: i32,
    pub crop_w: usize,
    pub crop_h: usize,
    pub png: Vec<u8>,
    pub force: bool,
}

pub type Reply = (u64, u64, u64, Result<Decision, String>);

#[derive(Debug)]
pub struct Placement {
    pub at: usize,
    pub text: String,
}

pub struct PlacementReply {
    pub id: u64,
    pub text_revision: u64,
    pub source_start: usize,
    pub source_end: usize,
    pub source: String,
    pub raw_result: String,
    pub result: Result<Placement, String>,
}

impl Oracle {
    /// Lightly present a raw program result and choose its most natural
    /// insertion point. Rat diagnostics have already been removed locally.
    pub fn place_result(
        &self,
        id: u64,
        text_revision: u64,
        document: String,
        source_start: usize,
        source_end: usize,
        source: String,
        selector: String,
        result: String,
        error: bool,
        tx: Sender<PlacementReply>,
    ) {
        let this = self.clone();
        std::thread::spawn(move || {
            let placed = this.place_result_sync(
                &document,
                source_start,
                source_end,
                &source,
                &selector,
                &result,
                error,
            );
            let _ = tx.send(PlacementReply {
                id,
                text_revision,
                source_start,
                source_end,
                source,
                raw_result: result,
                result: placed,
            });
        });
    }

    fn place_result_sync(
        &self,
        document: &str,
        source_start: usize,
        source_end: usize,
        source: &str,
        selector: &str,
        result: &str,
        error: bool,
    ) -> Result<Placement, String> {
        let system = "You place a program result into a paper-like document. Return exactly PLACE:S:LINE:T, where S is one Unicode character insertion offset and T is only the lightly presented result. Results are always separate lines so inserting one can never alter or invalidate Python source. Never return INLINE. Choose an offset after the complete source statement or block, not inside its indentation-sensitive code. Never place bare text directly against an existing character. Do not include runtime names, arrows, timing, variable counts, commentary, or labels such as Result. Preserve the complete useful value and its structure; do not explain, substantially summarize, truncate, omit rows, or collapse lines merely to save space. Results may naturally wrap and occupy multiple full pages. Preserve an [[INKTYPE_IMAGE:...]] or [[INKTYPE_MATH:...]] line exactly. Encode line breaks as <NL>. Usually insert immediately after the source expression, assignment, code block, or question, but you may choose another location when the page clearly implies it. This is insertion-only: never replace source text. For errors, use LINE and show only the concise useful error.";
        let prompt = format!(
            "DOCUMENT={document:?}\nLEN={}\nSOURCE_RANGE={source_start}..{source_end}\nSOURCE={source:?}\nRUNTIME={selector:?}\nERROR={error}\nRAW_RESULT={result:?}",
            document.chars().count()
        );
        let mut body = json!({
            "model": self.model,
            "stream": false,
            "temperature": 0,
            "reasoning_effort": "none",
            "max_tokens": 2048,
            "messages": [
                {"role":"system", "content":system},
                {"role":"user", "content":prompt}
            ]
        });
        if !self.base.contains("api.groq.com") {
            body.as_object_mut().unwrap().insert(
                "chat_template_kwargs".into(),
                json!({"enable_thinking": false}),
            );
        }
        let response = self
            .agent
            .post(&format!("{}/chat/completions", self.base))
            .set("Authorization", &format!("Bearer {}", self.key))
            .set("Content-Type", "application/json")
            .send_json(body)
            .map_err(|e| format!("result placement request: {e}"))?
            .into_string()
            .map_err(|e| format!("read result placement: {e}"))?;
        let value: Value = serde_json::from_str(&response)
            .map_err(|e| format!("result placement JSON: {e}"))?;
        let raw = value
            .pointer("/choices/0/message/content")
            .and_then(Value::as_str)
            .ok_or_else(|| format!("no result placement content: {value}"))?
            .trim();
        let rest = raw
            .strip_prefix("PLACE:")
            .ok_or_else(|| format!("bad result placement: {raw:?}"))?;
        let mut fields = rest.splitn(3, ':');
        let at = fields.next().unwrap_or_default();
        let style = fields.next().unwrap_or_default().trim();
        let presented = fields
            .next()
            .ok_or_else(|| format!("bad result placement: {raw:?}"))?
            .replace("<NL>", "\n");
        let at: usize = at
            .trim()
            .parse()
            .map_err(|_| format!("bad placement offset: {at:?}"))?;
        let chars: Vec<char> = document.chars().collect();
        if at > chars.len() {
            return Err(format!("unsafe placement offset {at}"));
        }
        if !matches!(style, "LINE" | "INLINE") {
            return Err(format!("bad placement style: {style:?}"));
        }
        // Enforce this locally even if the model disobeys and asks for INLINE.
        let prefix = if at > 0 && chars[at - 1] != '\n' { "\n" } else { "" };
        let suffix = if at < chars.len() && chars[at] != '\n' { "\n" } else { "" };
        let text = format!("{prefix}{}{suffix}", presented.trim());
        Ok(Placement { at, text })
    }
}

pub fn parse_decision(
    raw: &str,
    pending: usize,
    text_len: usize,
    anchor: usize,
) -> Result<Decision, String> {
    let trimmed = raw.trim();
    // Remove only a model-added protocol wrapper. `trim_matches('`')` used
    // to eat legitimate closing fences from replacement code.
    let line = if let Some(fenced) = trimmed
        .strip_prefix("```text\n")
        .or_else(|| trimmed.strip_prefix("```\n"))
        .and_then(|body| body.strip_suffix("```"))
    {
        fenced.trim()
    } else if let Some(inline) = trimmed
        .strip_prefix('`')
        .and_then(|body| body.strip_suffix('`'))
    {
        inline.trim()
    } else {
        trimmed
    };
    // Small models sometimes copy the protocol metavariable literally. The
    // pending count is local and authoritative, so substitute it safely.
    let normalized;
    let line = if line.starts_with("RAT:N,")
        || line.starts_with("RUN:N,")
        || line.starts_with("DATA:N,")
        || line.starts_with("MATH:N,")
        || line.starts_with("MATHCALC:N,")
    {
        normalized = line.replacen(":N,", &format!(":{pending},"), 1);
        normalized.as_str()
    } else {
        line
    };
    if line == "?" {
        return Ok(Decision::Wait);
    }
    if let Some(payload) = line.strip_prefix("PATCH:") {
        // Some models echo the protocol metavariable before the actual array.
        // Accept `PATCH:JSON\n[...]` as well as the requested `PATCH:[...]`.
        let payload = payload.trim();
        let payload = payload
            .strip_prefix("JSON")
            .map(str::trim)
            .unwrap_or(payload);
        let values: Value = serde_json::from_str(payload)
            .map_err(|e| format!("paper edit JSON: {e}"))?;
        let array = values
            .as_array()
            .ok_or_else(|| "paper edit JSON must be an array".to_string())?;
        let mut edits = Vec::new();
        for value in array {
            let old_text = value
                .get("old_text")
                .and_then(Value::as_str)
                .ok_or_else(|| "paper edit needs old_text".to_string())?;
            let new_text = value
                .get("new_text")
                .and_then(Value::as_str)
                .ok_or_else(|| "paper edit needs new_text".to_string())?;
            if old_text.is_empty() {
                return Err("paper edit old_text cannot be empty".into());
            }
            edits.push((old_text.to_string(), new_text.to_string()));
        }
        if edits.is_empty() {
            return Err("paper edit list cannot be empty".into());
        }
        return Ok(Decision::PaperEdits { edits });
    }
    if let Some(rest) = line.strip_prefix("EDIT:") {
        let mut parts = rest.splitn(4, ':');
        let consume = parts.next().unwrap_or_default();
        let start = parts.next().unwrap_or_default();
        let end = parts.next().unwrap_or_default();
        let replacement = parts.next().unwrap_or_default();
        let normalized = format!("{consume},{start},{end}:{replacement}");
        return parse_decision(&normalized, pending, text_len, anchor);
    }
    // Small vision models occasionally wrap a valid MATH response inside the
    // ordinary edit protocol (`N,S,E:MATH:...` or `<NL>MATH:...`). Recover the
    // typed protocol instead of inserting its implementation text visibly.
    if !line.starts_with("MATH:") {
        if let Some(offset) = line.find("MATH:") {
            return parse_decision(&line[offset..], pending, text_len, anchor);
        }
    }
    if let Some(fields) = line.strip_prefix("MATHCALC:") {
        let nums: Vec<usize> = fields
            .split(',')
            .map(|n| n.trim().parse().map_err(|_| format!("bad number {n:?}")))
            .collect::<Result<_, _>>()?;
        let [consume, start, end] = nums.as_slice() else {
            return Err(format!("bad math calculation protocol: {line:?}"));
        };
        if *consume == 0 || *consume > pending || start > end || *end > text_len {
            return Err(format!("unsafe math calculation protocol: {line:?}"));
        }
        return Ok(Decision::MathCalculate {
            consume: *consume,
            start: *start,
            end: *end,
        });
    }
    if let Some(rest) = line.strip_prefix("MATH:") {
        let (fields, payload) = rest
            .split_once(':')
            .ok_or_else(|| format!("bad math protocol: {line:?}"))?;
        let nums: Vec<usize> = fields
            .split(',')
            .map(|n| n.trim().parse().map_err(|_| format!("bad number {n:?}")))
            .collect::<Result<_, _>>()?;
        let [model_consume, _model_start, _model_end] = nums.as_slice() else {
            return Err(format!("bad math protocol: {line:?}"));
        };
        // In explicit math mode the local pending count and insertion anchor
        // are authoritative. Vision models frequently put zero or the gesture
        // count in the offset fields even when the transcription is correct.
        let consume = if *model_consume == 0 { pending } else { *model_consume };
        let (start, end) = (anchor, anchor);
        let value: Value = serde_json::from_str(payload).map_err(|e| format!("math JSON: {e}"))?;
        let latex = value
            .get("latex")
            .and_then(Value::as_str)
            .ok_or_else(|| "math JSON needs latex string".to_string())?;
        let semantic = value
            .get("semantic")
            .and_then(Value::as_str)
            .ok_or_else(|| "math JSON needs semantic string".to_string())?;
        if consume == 0 || consume > pending || start > text_len {
            return Err(format!("unsafe math protocol: {line:?}"));
        }
        return Ok(Decision::Math {
            consume,
            start,
            end,
            latex: latex.to_string(),
            semantic: semantic.to_string(),
        });
    }
    if let Some(rest) = line.strip_prefix("DATA:") {
        let mut parts = rest.splitn(4, ':');
        let fields = parts.next().unwrap_or_default();
        let runtime = parts.next().unwrap_or_default().trim();
        let name = parts.next().unwrap_or_default().trim();
        let json = parts.next().unwrap_or_default().trim();
        let nums: Vec<usize> = fields
            .split(',')
            .map(|n| n.trim().parse().map_err(|_| format!("bad number {n:?}")))
            .collect::<Result<_, _>>()?;
        let (consume, start, end) = match nums.as_slice() {
            [start, end] => (pending, *start, *end),
            [consume, start, end] => (if *consume == 0 { pending } else { *consume }, *start, *end),
            _ => return Err(format!("bad data protocol: {line:?}")),
        };
        if consume > pending
            || start > end
            || end > text_len
            || !matches!(runtime, "py" | "r")
            || !crate::repl::valid_variable(name)
            || serde_json::from_str::<Value>(json).is_err()
        {
            return Err(format!("unsafe data protocol: {line:?}"));
        }
        return Ok(Decision::Data {
            consume,
            start,
            end,
            runtime: runtime.to_string(),
            name: name.to_string(),
            json: json.to_string(),
        });
    }
    if let Some(rest) = line.strip_prefix("RAT:") {
        let (fields, selector) = rest
            .split_once(':')
            .ok_or_else(|| format!("bad rat protocol: {line:?}"))?;
        let nums: Vec<usize> = fields
            .split(',')
            .map(|n| n.trim().parse().map_err(|_| format!("bad number {n:?}")))
            .collect::<Result<_, _>>()?;
        let (consume, start, end) = match nums.as_slice() {
            [start, end] => (pending, *start, *end),
            [consume, start, end] => (if *consume == 0 { pending } else { *consume }, *start, *end),
            _ => return Err(format!("bad rat protocol: {line:?}")),
        };
        let selector = selector.trim();
        if consume > pending
            || start > end
            || end > text_len
            || !crate::repl::valid_selector(selector)
        {
            return Err(format!(
                "unsafe rat protocol: {line:?} pending={pending} text_len={text_len}"
            ));
        }
        return Ok(Decision::Rat {
            consume,
            start,
            end,
            selector: selector.to_string(),
        });
    }
    if let Some(fields) = line.strip_prefix("RUN:") {
        let nums: Vec<usize> = fields
            .split(',')
            .map(|n| n.trim().parse().map_err(|_| format!("bad number {n:?}")))
            .collect::<Result<_, _>>()?;
        let (consume, start, end) = match nums.as_slice() {
            [start, end] => (pending, *start, *end),
            [consume, start, end] => (if *consume == 0 { pending } else { *consume }, *start, *end),
            _ => return Err(format!("bad run protocol: {line:?}")),
        };
        if consume > pending || start > end || end > text_len {
            return Err(format!(
                "unsafe run protocol: {line:?} pending={pending} text_len={text_len}"
            ));
        }
        return Ok(Decision::Run {
            consume,
            start,
            end,
        });
    }
    let (head, text) = line
        .split_once(':')
        .ok_or_else(|| format!("bad protocol: {line:?}"))?;
    let nums: Vec<usize> = head
        .split(',')
        .map(|n| n.trim().parse().map_err(|_| format!("bad number {n:?}")))
        .collect::<Result<_, _>>()?;
    let (model_consume, mut start, mut end) = match nums.as_slice() {
        [n] => (*n, anchor, anchor),
        [n, s] => (*n, *s, *s),
        [n, s, e] => (*n, *s, *e),
        _ => return Err(format!("wrong field count: {line:?}")),
    };
    // Vision models occasionally count a surrounding quote or cursor cell and
    // overshoot the document by one. The intended edit is still unambiguous at
    // the right edge; clamp a tiny overshoot rather than poisoning all future
    // context by retaining the already-recognized ink.
    if start > text_len && start <= text_len + 2 {
        start = text_len;
    }
    if end > text_len && end <= text_len + 2 {
        end = text_len;
    }
    // The local pending queue is authoritative. Small/quantized vision models
    // often emit zero, a character offset, or a previous pending count in N.
    // Correct impossible values rather than retaining already-recognized ink.
    let consume = if model_consume == 0 || model_consume > pending {
        pending
    } else {
        model_consume
    };
    if pending == 0 || start > end || end > text_len {
        return Err(format!(
            "unsafe protocol: {line:?} pending={pending} text_len={text_len} anchor={anchor}"
        ));
    }
    // A dedicated token keeps structural newlines distinct from a literal
    // `\\n` inside Python source.
    let mut replacement = text.replace("<NL>", "\n");
    // Pen input never performs deletion (the eraser path is local). An empty
    // transcription therefore means the stroke is not understood yet.
    if replacement.is_empty() {
        return Ok(Decision::Wait);
    }
    if replacement.len() >= 2 && replacement.starts_with('"') && replacement.ends_with('"') {
        replacement = replacement[1..replacement.len() - 1].to_string();
    }
    Ok(Decision::Edit {
        consume,
        start,
        end,
        text: replacement,
    })
}

fn debug_write(slot: u64, extension: &str, data: &[u8]) {
    let dir =
        std::env::var("INKTYPE_DEBUG_DIR").unwrap_or_else(|_| "/home/root/inktype-debug".into());
    if std::fs::create_dir_all(&dir).is_ok() {
        let _ = std::fs::write(format!("{dir}/slot-{slot:02}.{extension}"), data);
    }
}

fn base64(data: &[u8]) -> String {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(data.len().div_ceil(3) * 4);
    for c in data.chunks(3) {
        let b = [c[0], *c.get(1).unwrap_or(&0), *c.get(2).unwrap_or(&0)];
        let n = ((b[0] as u32) << 16) | ((b[1] as u32) << 8) | b[2] as u32;
        out.push(T[((n >> 18) & 63) as usize] as char);
        out.push(T[((n >> 12) & 63) as usize] as char);
        out.push(if c.len() > 1 {
            T[((n >> 6) & 63) as usize] as char
        } else {
            '='
        });
        out.push(if c.len() > 2 {
            T[(n & 63) as usize] as char
        } else {
            '='
        });
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn protocol() {
        assert_eq!(parse_decision("?", 2, 5, 3).unwrap(), Decision::Wait);
        assert_eq!(
            parse_decision("EDIT:2:1:4:fixed", 2, 5, 0).unwrap(),
            Decision::Edit {
                consume: 2,
                start: 1,
                end: 4,
                text: "fixed".into(),
            }
        );
        assert_eq!(
            parse_decision(r#"MATH:2,1,1:{"latex":"x^2","semantic":"x**2"}"#, 2, 5, 0,).unwrap(),
            Decision::Math {
                consume: 2,
                start: 0,
                end: 0,
                latex: "x^2".into(),
                semantic: "x**2".into(),
            }
        );
        assert_eq!(
            parse_decision(
                r#"2,1,1:<NL>MATH:2,1,1:{"latex":"x^2","semantic":"x**2"}"#,
                2,
                5,
                0,
            )
            .unwrap(),
            Decision::Math {
                consume: 2,
                start: 0,
                end: 0,
                latex: "x^2".into(),
                semantic: "x**2".into(),
            }
        );
        assert_eq!(
            parse_decision("MATHCALC:2,0,5", 2, 5, 0).unwrap(),
            Decision::MathCalculate {
                consume: 2,
                start: 0,
                end: 5,
            }
        );
        assert_eq!(
            parse_decision(
                r#"DATA:2,1,4:py:sales:{"columns":["x"],"rows":[[1]]}"#,
                2,
                5,
                0,
            )
            .unwrap(),
            Decision::Data {
                consume: 2,
                start: 1,
                end: 4,
                runtime: "py".into(),
                name: "sales".into(),
                json: r#"{"columns":["x"],"rows":[[1]]}"#.into(),
            }
        );
        assert_eq!(
            parse_decision("RAT:2,1,4:my-py-here", 2, 5, 0).unwrap(),
            Decision::Rat {
                consume: 2,
                start: 1,
                end: 4,
                selector: "my-py-here".into()
            }
        );
        assert_eq!(
            parse_decision("RAT:N,1,4:py", 3, 5, 0).unwrap(),
            Decision::Rat {
                consume: 3,
                start: 1,
                end: 4,
                selector: "py".into()
            }
        );
        assert!(parse_decision("RAT:2,1,4:py;reboot", 2, 5, 0).is_err());
        assert_eq!(
            parse_decision("RUN:1,4", 3, 5, 0).unwrap(),
            Decision::Run {
                consume: 3,
                start: 1,
                end: 4
            }
        );
        assert_eq!(
            parse_decision("RUN:2,1,4", 2, 5, 0).unwrap(),
            Decision::Run {
                consume: 2,
                start: 1,
                end: 4
            }
        );
        assert_eq!(
            parse_decision("2,1,2:t", 2, 5, 0).unwrap(),
            Decision::Edit {
                consume: 2,
                start: 1,
                end: 2,
                text: "t".into()
            }
        );
        assert_eq!(parse_decision("1,1,3:", 2, 5, 0).unwrap(), Decision::Wait);
        assert_eq!(
            parse_decision("2:hi", 2, 5, 3).unwrap(),
            Decision::Edit {
                consume: 2,
                start: 3,
                end: 3,
                text: "hi".into()
            }
        );
        assert_eq!(
            parse_decision("1,4:x", 2, 5, 0).unwrap(),
            Decision::Edit {
                consume: 1,
                start: 4,
                end: 4,
                text: "x".into()
            }
        );
        assert_eq!(
            parse_decision("3,0,0:x", 2, 5, 0).unwrap(),
            Decision::Edit {
                consume: 2,
                start: 0,
                end: 0,
                text: "x".into(),
            }
        );
        assert_eq!(
            parse_decision("1,0,0:```python<NL>print(1)<NL>```", 1, 0, 0).unwrap(),
            Decision::Edit {
                consume: 1,
                start: 0,
                end: 0,
                text: "```python\nprint(1)\n```".into()
            }
        );
        assert_eq!(
            parse_decision("1,0,0:print(\\\"\\\\n\\\")", 1, 0, 0).unwrap(),
            Decision::Edit {
                consume: 1,
                start: 0,
                end: 0,
                text: "print(\\\"\\\\n\\\")".into()
            }
        );
        assert_eq!(
            parse_decision("```text\n1,0,0:ok\n```", 1, 0, 0).unwrap(),
            Decision::Edit {
                consume: 1,
                start: 0,
                end: 0,
                text: "ok".into()
            }
        );
        assert_eq!(
            parse_decision("2,0,7:\"hello\"", 2, 5, 0).unwrap(),
            Decision::Edit {
                consume: 2,
                start: 0,
                end: 5,
                text: "hello".into()
            }
        );
    }
}
