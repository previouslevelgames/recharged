"""Readable formatting of prepared C/C++ sources that provably compiles the same.

clang-format changes layout, and layout is not always invisible to the
compiler. __LINE__ (assertion and panic messages of the matching SDK code,
whose values equal retail), stringified macro arguments (#x keeps the spacing
of its argument) and the space between a macro name and its parameter list all
depend on it. Every formatted file is lexed into preprocessing tokens and
compared with its input. A file is used formatted only when its tokens,
directives and comments are unchanged and every layout-dependent construct
keeps its layout; otherwise it is kept exactly as patched.
"""
from bisect import bisect_right
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

# C/C++ files formatted in prepared trees; everything else is copied unchanged.
FORMAT_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".inc")
MINIMUM_CLANG_FORMAT = 16
# Dependencies formatted when the preparer is run directly without --clang-format.
FORMATTED_BY_DEFAULT = frozenset({"mscharged-decomp"})

# Macros defined outside the prepared tree that stringify their arguments
# and report __LINE__ (the host C library and SDL assertion macros).
EXTERNAL_LINE_MACROS = frozenset({
    "assert", "SDL_assert", "SDL_assert_release", "SDL_assert_paranoid", "SDL_assert_always",
})
EXTERNAL_STRINGIFYING_MACROS = EXTERNAL_LINE_MACROS
LINE_TOKENS = frozenset({"__LINE__", "__builtin_LINE"})
# Files using these report source positions that formatting would change.
POSITION_TOKENS = frozenset({"__builtin_COLUMN", "source_location"})

_SPACE = "[ \\t\\r\\v\\f]*"
_IDENT_CHAR = "0-9A-Za-z_$\\x80-\\xff"
_SUFFIX = f"(?:[A-Za-z_$\\x80-\\xff][{_IDENT_CHAR}]*)?"
# One match per token, including the whitespace before it. The lexer merges
# at least as much as a real C/C++ lexer, so equal token sequences cannot hide
# a change in the compiler's tokens.
_TOKEN = re.compile(
    f"(?P<space>{_SPACE})(?:"
    "(?P<nl>\\n)"
    "|(?P<lc>//[^\\n]*)"
    "|(?P<bc>/\\*.*?\\*/)"
    f"|(?P<raw>(?:u8|[uUL])?R\"(?P<delim>[^()\\\\ \\t\\v\\f\\n]{{0,16}})\\(.*?\\)(?P=delim)\"{_SUFFIX})"
    f"|(?P<str>(?:u8|[uUL])?(?:\"(?:[^\"\\\\\\n]|\\\\.)*\"|'(?:[^'\\\\\\n]|\\\\.)*'){_SUFFIX})"
    f"|(?P<num>\\.?[0-9](?:[eEpP][+-]|'[0-9A-Za-z_]|[{_IDENT_CHAR}.])*)"
    f"|(?P<id>[A-Za-z_$\\x80-\\xff][{_IDENT_CHAR}]*)"
    "|(?P<op>%:%:|\\.\\.\\.|<<=|>>=|->\\*|<=>|##|<:|:>|<%|%>|%:|::|->|\\.\\*|\\+\\+|--|<<|>>|<=|>=|==|!="
    "|&&|\\|\\||[-+*/%&|^]=|.)"
    f"|(?P<end>{_SPACE}$)"
    ")",
    re.DOTALL,
)
_HEADER_NAME = re.compile(f"(?m)^{_SPACE}(?:#|%:){_SPACE}(?:include|include_next|import){_SPACE}(<[^>\\n]*>)")
_HAS_INCLUDE = re.compile(f"__has_include(?:_next)?{_SPACE}\\({_SPACE}(<[^>\\n]*>)")
_SPLICE = re.compile("\\\\\\r?\\n")
_SQUASH = re.compile(rb"\\\r?\n|\s+")
# Logical #define lines, optionally after block comments on the same line.
_DEFINE_LINE = re.compile(f"(?m)^(?:[ \\t]|/\\*.*?\\*/)*(?:#|%:){_SPACE}define\\b[^\\n]*")
_DIRECTIVE_MARK = ("#", "%:")


def _squash(data: bytes) -> bytes:
    return _SQUASH.sub(b"", data)


def _definition_lines(data: bytes) -> bytes:
    """The #define directives of a file, which is all macro analysis needs."""
    if b"define" not in data:
        return b""
    text = _SPLICE.sub("", data.decode("latin-1"))
    found = [match.group() for match in _DEFINE_LINE.finditer(text)]
    if any(line.count("/*") > line.count("*/") for line in found):
        return data  # A comment continues the directive; analyse the whole file.
    return "\n".join(found).encode("latin-1")


class Lexed:
    """Preprocessing tokens of one file.

    texts[i], lines[i] (physical line), spaced[i] (whitespace or a comment
    before the token), directive[i] (0 outside directives, else a running
    directive number) and the whitespace-free comment texts."""
    __slots__ = ("texts", "lines", "spaced", "directive", "comments", "kinds")

    def __init__(self):
        self.texts, self.lines, self.spaced, self.directive, self.comments = [], [], [], [], []
        self.kinds = {0: ""}  # directive number -> keyword (define, if, line, ...)


def lex(data: bytes) -> Lexed:
    text = data.decode("latin-1")
    splices = []
    if "\\" in text:
        parts, start, spliced = [], 0, 0
        for match in _SPLICE.finditer(text):
            parts.append(text[start:match.start()])
            spliced += match.start() - start
            splices.append(spliced)  # Position in the spliced text of each removed line break.
            start = match.end()
        parts.append(text[start:])
        text = "".join(parts)
    headers = {match.start(1): match.end(1) for match in _HEADER_NAME.finditer(text)}
    headers.update((match.start(1), match.end(1)) for match in _HAS_INCLUDE.finditer(text))
    result = Lexed()
    texts, lines, spaced, directive, comments = (result.texts, result.lines, result.spaced,
                                                result.directive, result.comments)
    newlines = 0
    pending_space = False
    line_start = True
    current = 0
    directives = 0
    position, length = 0, len(text)
    match_token = _TOKEN.match
    while position < length:
        match = match_token(text, position)
        kind = match.lastgroup
        if match.end("space") > position:
            pending_space = True
        if kind == "end":
            break
        if kind == "nl":
            newlines += 1
            pending_space = True
            line_start = True
            current = 0
            position = match.end()
            continue
        if kind in ("lc", "bc"):
            comment = match.group(kind)
            comments.append("".join(comment.split()))
            newlines += comment.count("\n")
            pending_space = True
            position = match.end()
            continue
        start = match.start(kind)
        end = headers.get(start)
        if end is not None:
            token = text[start:end]
        else:
            token = match.group(kind)
            end = match.end()
        if line_start and token in _DIRECTIVE_MARK:
            directives += 1
            current = directives
            result.kinds[current] = ""
        elif current and not result.kinds[current] and texts[-1] in _DIRECTIVE_MARK:
            result.kinds[current] = token
        texts.append(token)
        lines.append(1 + newlines + (bisect_right(splices, start) if splices else 0))
        spaced.append(pending_space)
        directive.append(current)
        newlines += token.count("\n")
        pending_space = False
        line_start = False
        position = end
    return result


def _definitions(lexed: Lexed):
    """Yield (name, parameters or None, body token indices) for each #define."""
    texts, spaced, directive = lexed.texts, lexed.spaced, lexed.directive
    count = len(texts)
    index = 0
    while index < count:
        number = directive[index]
        if number and lexed.kinds[number] == "define" and texts[index] in _DIRECTIVE_MARK \
                and (index == 0 or directive[index - 1] != number) \
                and index + 2 < count and directive[index + 2] == number:
            end = index + 3
            while end < count and directive[end] == number:
                end += 1
            name = texts[index + 2]
            body = index + 3
            parameters = None
            if body < end and texts[body] == "(" and not spaced[body]:
                parameters = []
                body += 1
                while body < end and texts[body] != ")":
                    if texts[body] != ",":
                        parameters.append(texts[body])
                    body += 1
                body += 1
            yield index + 2, name, parameters, range(min(body, end), end)
            index = end
        else:
            index += 1


def _closure(seed, bodies):
    """Macros whose bodies refer, possibly indirectly, to a member of seed."""
    found = set(seed)
    changed = True
    while changed:
        changed = False
        for name, identifiers in bodies.items():
            if name not in found and not identifiers.isdisjoint(found):
                found.add(name)
                changed = True
    return found


def analyse(files):
    """Find layout-dependent macros of a tree. files: iterable of bytes."""
    files = list(files)
    bodies, direct = {}, set()
    for data in files:
        definitions = _definition_lines(data)
        if not definitions:
            continue
        lexed = lex(definitions)
        texts = lexed.texts
        for _, name, parameters, body in _definitions(lexed):
            identifiers = bodies.setdefault(name, set())
            identifiers.update(texts[i] for i in body)
            if parameters is not None:
                if "..." in parameters:
                    parameters.append("__VA_ARGS__")
                for i in body:
                    if texts[i] in _DIRECTIVE_MARK and i + 1 in body and texts[i + 1] in parameters:
                        direct.add(name)
    line = _closure(LINE_TOKENS | EXTERNAL_LINE_MACROS, bodies) - LINE_TOKENS
    stringifying = _closure(direct | EXTERNAL_STRINGIFYING_MACROS, bodies)
    # Macros that hand their arguments to a stringifying macro expand them first,
    # so the spacing inside the expanded macros' bodies reaches the string.
    expanding = {name for name, identifiers in bodies.items() if not identifiers.isdisjoint(stringifying)}
    expanded = set()
    if expanding:
        uses = re.compile(b"\\b(?:" + b"|".join(re.escape(name.encode("latin-1")) for name in sorted(expanding))
                          + b")\\b")
        for data in files:
            if not uses.search(data):
                continue
            texts = lex(data).texts
            for index, token in enumerate(texts):
                if token in expanding:
                    expanded.update(texts[i] for i in _invocation(texts, index) if texts[i] in bodies)
    pending = list(expanded)
    while pending:
        for name in bodies[pending.pop()] & bodies.keys():
            if name not in expanded:
                expanded.add(name)
                pending.append(name)
    return {"line": sorted(line), "stringifying": sorted(stringifying), "expanded": sorted(expanded)}


def _invocation(texts, index):
    """Token indices inside the parentheses following texts[index], if any."""
    count = len(texts)
    if index + 1 >= count or texts[index + 1] != "(":
        return range(0)
    depth = 0
    for end in range(index + 1, count):
        if texts[end] == "(":
            depth += 1
        elif texts[end] == ")":
            depth -= 1
            if depth == 0:
                return range(index + 2, end + 1)
    return range(index + 2, count)


_OPEN = frozenset({"(", "[", "<:"})
_CLOSE = frozenset({")", "]", ":>"})
_STATEMENT_END = frozenset({";", "{", "}", "<%", "%>"})


def _statement(lexed: Lexed, index: int):
    """Token indices of the statement around texts[index], with whole bracket groups."""
    texts, directive = lexed.texts, lexed.directive
    region = directive[index]
    count = len(texts)
    depth = 0
    start = index
    while start > 0:
        if directive[start - 1] != region:
            if region:
                break
            start -= 1  # A directive inside a statement does not end it.
            continue
        token = texts[start - 1]
        if token in _CLOSE:
            depth += 1
        elif token in _OPEN:
            depth -= 1
        elif token in _STATEMENT_END and depth <= 0:
            break
        start -= 1
    depth = 0
    end = start
    while end < count:
        if directive[end] != region:
            if region:
                break
            end += 1
            continue
        token = texts[end]
        if token in _OPEN:
            depth += 1
        elif token in _CLOSE:
            depth -= 1
        elif token in _STATEMENT_END and depth <= 0 and end >= index:
            break
        end += 1
    return [i for i in range(start, min(end + 1, count)) if directive[i] == region]


def verify(raw: Lexed, formatted: Lexed, analysis) -> str | None:
    """None when formatted compiles exactly like raw, else the reason it may not."""
    if raw.texts != formatted.texts:
        return "tokens changed"
    if raw.directive != formatted.directive:
        return "preprocessing directives changed"
    if raw.comments != formatted.comments:
        return "comments changed"
    if "line" in raw.kinds.values() and raw.lines != formatted.lines:
        return "#line directive"
    texts = raw.texts
    if not POSITION_TOKENS.isdisjoint(texts):
        return "source position builtin"  # Reports lines and columns.
    line_macros = analysis["line"]
    stringifying = analysis["stringifying"]
    expanded = analysis["expanded"]
    raw_lines, formatted_lines = raw.lines, formatted.lines
    raw_spaced, formatted_spaced = raw.spaced, formatted.spaced
    if raw_spaced == formatted_spaced and raw_lines == formatted_lines:
        return None
    for name_index, name, parameters, body in _definitions(raw):
        after = name_index + 1
        if after < len(texts) and raw.directive[after] == raw.directive[name_index] \
                and raw_spaced[after] != formatted_spaced[after]:
            return f"#define {name} spacing"
        if name in expanded and any(raw_spaced[i] != formatted_spaced[i] for i in body[1:]):
            return f"#define {name} body spacing"
    kinds = raw.kinds
    for index, token in enumerate(texts):
        if token in LINE_TOKENS or token in line_macros:
            if kinds[raw.directive[index]] in ("define", "undef"):
                continue  # Expanded where the macro is used, not here.
            if any(raw_lines[i] != formatted_lines[i] for i in _statement(raw, index)):
                return f"{token} line moved"
        if token in stringifying:
            region = _invocation(texts, index) or _statement(raw, index)
            if any(raw_spaced[i] != formatted_spaced[i] for i in region):
                return f"{token} argument spacing"
    return None


_ANALYSIS = None


def _set_analysis(sets):
    global _ANALYSIS
    _ANALYSIS = sets


def _verify_pair(pair):
    original, formatted = pair
    return verify(lex(original), lex(formatted), _ANALYSIS)


def prune_cache(cache: Path, used):
    """Keep only the cache entries of the current tree."""
    if cache.is_dir():
        for path in cache.rglob("*"):
            if path.is_file() and path not in used:
                path.unlink()


def find_clang_format():
    """A clang-format new enough for the formatting definition, or None."""
    names = ["clang-format"] + [f"clang-format-{version}" for version in range(30, MINIMUM_CLANG_FORMAT - 1, -1)]
    for name in names:
        path = shutil.which(name)
        if path is None:
            continue
        try:
            output = subprocess.run([path, "--version"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                    check=True).stdout.decode()
        except (OSError, subprocess.CalledProcessError):
            continue
        match = re.search(r"clang-format version (\d+)", output)
        if match and int(match.group(1)) >= MINIMUM_CLANG_FORMAT:
            return path
    return None


def formatter_identity(clang_format: str, config: Path):
    """Identity of the formatting stage: tool version, definition and verifier."""
    try:
        version = subprocess.run([clang_format, "--version"], stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, check=True).stdout.decode().strip()
    except (OSError, subprocess.CalledProcessError) as error:
        raise RuntimeError(f"Cannot run clang-format: {clang_format}") from error
    match = re.search(r"clang-format version (\d+)", version)
    if not match or int(match.group(1)) < MINIMUM_CLANG_FORMAT:
        raise RuntimeError(f"Formatting prepared sources requires clang-format {MINIMUM_CLANG_FORMAT} "
                           f"or newer; found: {version or clang_format}")
    if not config.is_file():
        raise RuntimeError(f"Missing formatting definition: {config}")
    check = subprocess.run([clang_format, f"--style=file:{config}", "--dump-config"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if check.returncode != 0:
        raise RuntimeError(f"clang-format rejected {config}: " + check.stderr.decode(errors="replace").strip())
    return {"tool": version, "config_sha256": hashlib.sha256(config.read_bytes()).hexdigest(),
            "suffixes": list(FORMAT_SUFFIXES),
            "verifier_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}


def format_tree(tree: Path, clang_format: str, config: Path, identity, cache: Path, jobs: int):
    """Format the C/C++ files of tree in place where verification allows.

    Returns (cache entries used, {path: reason} for files kept unformatted).
    Decisions are cached by input content, formatter identity and the tree's
    layout-dependent macros, so a focused patch only formats what it changes."""
    contents = {path: path.read_bytes() for path in sorted(tree.rglob("*"))
                if path.is_file() and not path.is_symlink()}
    candidates = [(path, data) for path, data in contents.items() if path.suffix.lower() in FORMAT_SUFFIXES]
    analysis = analyse(contents.values())
    sets = {key: frozenset(value) for key, value in analysis.items()}
    salt = json.dumps({"identity": identity, "analysis": analysis}, sort_keys=True).encode()
    used, kept, pending = set(), {}, []
    for path, original in candidates:
        digest = hashlib.sha256(salt + path.suffix.lower().encode() + b"\0" + original).hexdigest()
        entry = cache / digest[:2] / digest
        skip = entry.with_suffix(".kept")
        cached = entry.read_bytes() if entry.is_file() else None
        if cached is not None and _squash(cached) == _squash(original):
            used.add(entry)
            path.write_bytes(cached)
        elif skip.is_file():
            used.add(skip)
            kept[path.relative_to(tree).as_posix()] = skip.read_text()
        else:
            pending.append((path, original, entry))

    def run(batch):
        result = subprocess.run([clang_format, "-i", f"--style=file:{config}", *(str(p) for p, _, _ in batch)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode == 0:
            return []
        failed = []
        for item in batch:  # Find the files clang-format cannot process.
            item[0].write_bytes(item[1])
            single = subprocess.run([clang_format, "-i", f"--style=file:{config}", str(item[0])],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if single.returncode != 0:
                item[0].write_bytes(item[1])
                failed.append(item[0])
        return failed

    batches = [pending[i:i + 48] for i in range(0, len(pending), 48)]
    failed = set()
    with ThreadPoolExecutor(max_workers=max(1, jobs)) as executor:
        for paths in executor.map(run, batches):
            failed.update(paths)
    results = [(path, original, entry, path.read_bytes()) for path, original, entry in pending]
    changed = [(original, formatted) for path, original, _, formatted in results
               if path not in failed and formatted != original]
    if len(changed) > 32 and jobs > 1:
        with ProcessPoolExecutor(max_workers=jobs, initializer=_set_analysis, initargs=(sets,)) as executor:
            verdicts = iter(list(executor.map(_verify_pair, changed, chunksize=16)))
    else:
        _set_analysis(sets)
        verdicts = iter([_verify_pair(pair) for pair in changed])
    for path, original, entry, formatted in results:
        if path in failed:
            reason = "clang-format error"
        elif formatted == original:
            reason = None
        else:
            reason = next(verdicts)
        entry.parent.mkdir(parents=True, exist_ok=True)
        if reason is None:
            target, content = entry, formatted
        else:
            path.write_bytes(original)
            kept[path.relative_to(tree).as_posix()] = reason
            target, content = entry.with_suffix(".kept"), reason.encode()
        temporary = target.with_name(target.name + ".tmp")
        temporary.write_bytes(content)
        temporary.replace(target)
        used.add(target)
    return used, kept
