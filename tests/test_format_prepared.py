#!/usr/bin/env python3
"""Check that formatting is accepted only when the compiled program is unchanged."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from format_prepared import analyse, lex, verify


def check(raw: str, formatted: str, *headers: str):
    """verify() for raw -> formatted, with macros from headers and raw."""
    sources = [text.encode() for text in (*headers, raw)]
    sets = {key: frozenset(value) for key, value in analyse(sources).items()}
    return verify(lex(raw.encode()), lex(formatted.encode()), sets)


PANIC = "void report(int line);\n#define PANIC() report(__LINE__)\n"


class FormattingVerificationTests(unittest.TestCase):
    def test_layout_change_accepted(self):
        self.assertIsNone(check("int  f( int a ){return a+1;}\n",
                                "int f(int a)\n{\n    return a + 1;\n}\n"))

    def test_token_change_rejected(self):
        self.assertEqual(check("A<B<int> > x;\n", "A<B<int>> x;\n"), "tokens changed")
        self.assertEqual(check('const char* s = R"(a  b)";\n', 'const char* s = R"(a b)";\n'), "tokens changed")
        self.assertEqual(check("#include <a b.h>\n", "#include <ab.h>\n"), "tokens changed")
        self.assertEqual(check("int a<:1:>;\n", "int a< :1: >;\n"), "tokens changed")

    def test_line_splice_tokenization(self):
        # foo\<newline>bar is one identifier; a space before the splice makes two.
        self.assertEqual(check("#define X foo\\\nbar\n", "#define X foo \\\nbar\n"), "tokens changed")
        self.assertIsNone(check("#define X foo \\\n    bar\n", "#define X foo \\\n        bar\n"))

    def test_directive_structure(self):
        self.assertEqual(check("#define A 1\nint x;\n", "#define A 1 int x;\n"), "preprocessing directives changed")

    def test_comments(self):
        self.assertIsNone(check("int x; /*  note\n   continued */\n", "int x; /* note\n continued */\n"))
        self.assertEqual(check("int x; // note\n", "int x; // nope\n"), "comments changed")

    def test_function_like_macro_spacing(self):
        self.assertEqual(check("#define F(x) (x)\n", "#define F (x) (x)\n"), "#define F spacing")

    def test_line_macro_moves(self):
        raw = PANIC + "void f() { PANIC(); }\n"
        self.assertEqual(check(raw, PANIC + "void f()\n{\n    PANIC();\n}\n"), "PANIC line moved")
        self.assertIsNone(check(raw, PANIC + "void f() {  PANIC( ); }\n"))
        self.assertEqual(check("int f() { return __LINE__; }\n", "int f()\n{\n    return __LINE__;\n}\n"),
                         "__LINE__ line moved")

    def test_line_macro_through_other_macro_and_header(self):
        header = PANIC + "#define CHECK(x) if (!(x)) PANIC()\n"
        raw = "void f(int a) { CHECK(a); }\n"
        self.assertEqual(check(raw, "void f(int a)\n{\n    CHECK(a);\n}\n", header), "CHECK line moved")

    def test_line_in_multiline_invocation(self):
        raw = PANIC + "#define WRAP(x) x\nvoid f() { WRAP(\n PANIC()); }\n"
        formatted = PANIC + "#define WRAP(x) x\nvoid f() { WRAP(\n PANIC()\n); }\n"
        self.assertEqual(check(raw, formatted), "PANIC line moved")

    def test_line_macro_definition_may_move(self):
        # __LINE__ in a #define body expands where the macro is used.
        self.assertIsNone(check("int a;\n\n\n" + PANIC, "int a;\n\n" + PANIC))

    def test_line_directive(self):
        self.assertEqual(check("#line 100\nint  x;\n\n\nint y;\n", "#line 100\nint x;\n\nint y;\n"),
                         "#line directive")

    def test_stringified_argument_spacing(self):
        header = "void report(const char*);\n#define CHECK(e) report(#e)\n"
        self.assertEqual(check("void f(int a,int b) { CHECK(a+b); }\n",
                               "void f(int a, int b) { CHECK(a + b); }\n", header), "CHECK argument spacing")
        self.assertIsNone(check("void f(int a,int b) { CHECK(a + b); }\n",
                                "void f(int a, int b)\n{\n    CHECK(a + b);\n}\n", header))

    def test_variadic_stringification(self):
        self.assertEqual(check("#define S(...) #__VA_ARGS__\nconst char* s = S(a,b);\n",
                               "#define S(...) #__VA_ARGS__\nconst char* s = S(a, b);\n"), "S argument spacing")

    def test_stringified_expansion_body(self):
        raw = "#define STR(x) #x\n#define XSTR(x) STR(x)\n#define V 1+2\nconst char* v = XSTR(V);\n"
        formatted = raw.replace("1+2", "1 + 2")
        self.assertEqual(check(raw, formatted), "#define V body spacing")

    def test_host_assert(self):
        self.assertEqual(check("void f(int a) { assert(a+1); }\n", "void f(int a) { assert(a + 1); }\n"),
                         "assert argument spacing")
        self.assertEqual(check("void f(int a) { assert(a); }\n", "void f(int a)\n{\n    assert(a);\n}\n"),
                         "assert line moved")

    def test_physical_lines_after_splices(self):
        lexed = lex(b"#define A \\\n  1 \\\n  2 \\\r\n  3\nint x;\n")
        self.assertEqual([lexed.lines[lexed.texts.index(token)] for token in ("1", "2", "3", "x")], [2, 3, 4, 5])
        self.assertEqual(lexed.directive[lexed.texts.index("3")], 1)
        self.assertEqual(lexed.directive[lexed.texts.index("x")], 0)

    def test_source_position_builtins(self):
        # Columns change with indentation, so any layout change is rejected.
        self.assertEqual(check("int f() { return __builtin_COLUMN(); }\n", "int f() {  return __builtin_COLUMN(); }\n"),
                         "source position builtin")


if __name__ == "__main__":
    unittest.main()
