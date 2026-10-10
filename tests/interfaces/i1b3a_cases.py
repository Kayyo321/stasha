"""I1b.3a: negative-only immediate nominal contracts, not stored-history proof.

Controlling specification: I1B3-BOUNDARY-PLAN.md section 16, SHA-256
95722c33788da24088ab12a458f6c730e3e6ffebcf59fe3872cf44d7299f31ff.

Only ``valid`` cases may execute. Every ``reject`` and ``deferred`` case is
compile-only, even if it is unexpectedly accepted. Valid cases have exact zero
exit/empty output oracles at O0, O1, O2, and O3. ``deferred`` means an explicitly
unclosed boundary; compile acceptance is never a positive safety result.

The source inventory is intentionally independent of the prior I1b.2 oracles.
Expression markers are removed before compilation; their one-based coordinates
freeze the exact immediate operand. Seam coordinates identify the owning call
or ret token separately, without allowing a diagnostic elsewhere in the file.
"""

from dataclasses import dataclass, field
import hashlib
import re
from typing import Literal


Category = Literal["valid", "reject", "deferred"]
ARGUMENT_DIAGNOSTIC = "interface immediate argument: definite nominal mismatch"
RETURN_DIAGNOSTIC = "interface immediate return: definite nominal mismatch"
LEVELS = (0, 1, 2, 3)
BASELINE_SHA256 = "e6c28df52b705364b266c47635b2b9856d7fb08208001fa021ffc8fedd8d6db6"


@dataclass(frozen=True)
class Case:
    name: str
    source: str
    category: Category
    route: str
    source_contract: str
    boundary: str
    diagnostic: str | None = None
    expected_span: tuple[int, int] | None = None
    seam_span: tuple[int, int] | None = None
    files: dict[str, str] = field(default_factory=dict)
    expected_exit: int | None = None
    expected_stdout: str | None = None
    expected_stderr: str | None = None
    expected_diagnostic_file: str = "input.sts"

    @property
    def positive(self):
        return self.category == "valid"

    @property
    def execute(self):
        return self.category == "valid"

    @property
    def compile_only(self):
        return self.category != "valid"

    @property
    def pattern(self):
        return re.escape(self.diagnostic) if self.diagnostic else None

    @property
    def expected_line(self):
        return self.expected_span[0] if self.expected_span else None

    @property
    def expected_column(self):
        return self.expected_span[1] if self.expected_span else None

    @property
    def expected_diagnostic_span(self):
        # Named compound constructors retain a line but no original column.
        # This fixes the owned diagnostic fallback without a parser expansion.
        if self.category == "reject" and self.source_contract.endswith("named-construction"):
            return self.seam_span if self.route == "argument" else (self.expected_line, 1)
        return self.expected_span


I = "type I: interface { get(void): i32; };\n"
S = "type S: struct.[I] { ext i32 n; ext fn get(void): i32 { ret this.n; } };\n"
T = "type T: struct { ext i32 n; };\n"
G = "type G: @comptime[U] struct.[I] { ext U n; ext fn get(void): i32 { ret 7; } };\n"
LITERAL = "type G_G_i32: struct.[I] { ext i32 n; ext fn get(void): i32 { ret this.n; } };\n"
HEADER = "mod immediate;\n" + I + S + T + G + LITERAL
REVERSED_HEADER = "mod immediate;\n" + I + LITERAL + G + T + S


def _marked(text):
    """Strip internal markers while retaining exact source-token positions."""
    markers = {"operand": "/*@operand*/", "seam": "/*@seam*/"}
    positions = {}
    for key, marker in markers.items():
        if marker not in text:
            continue
        assert text.count(marker) == 1, (key, text)
        before = text[:text.index(marker)]
        for other in markers.values():
            before = before.replace(other, "")
        positions[key] = (before.count("\n") + 1, len(before.rsplit("\n", 1)[-1]) + 1)
    for marker in markers.values():
        text = text.replace(marker, "")
    return text, positions


def matrix():
    cases = []

    def add(name, body, category="valid", *, route="compatibility",
            source_contract="declared", boundary="existing behavior",
            header=HEADER, files=None, expected_diagnostic_file="input.sts"):
        source, positions = _marked(header + body.strip() + "\n")
        files = dict(files or {})
        if expected_diagnostic_file != "input.sts":
            assert expected_diagnostic_file in files, name
            assert not positions, "diagnostic markers must belong to the diagnostic source file"
            files[expected_diagnostic_file], positions = _marked(files[expected_diagnostic_file])
        diagnostic = None
        if category == "reject":
            assert route in ("argument", "return")
            assert "operand" in positions and "seam" in positions, name
            diagnostic = ARGUMENT_DIAGNOSTIC if route == "argument" else RETURN_DIAGNOSTIC
        cases.append(Case(name, source, category, route, source_contract, boundary,
                          diagnostic, positions.get("operand"), positions.get("seam"),
                          files, 0 if category == "valid" else None,
                          "" if category == "valid" else None,
                          "" if category == "valid" else None,
                          expected_diagnostic_file))

    # Admitted same-declaration contracts, independently of any boxing sink.
    for linkage, prefix in (("default", ""), ("internal", "int "), ("public", "ext ")):
        add("valid-exact-" + linkage, f"""
{prefix}fn take(stack S value): i32 {{ ret value.n; }}
{prefix}fn reflect(stack S value): S {{ ret value; }}
fn main(void): i32 {{ stack S value=.{{.n=41}}; ret take(reflect(value))-41; }}
""", route="argument+return", boundary="matching declarations only; no downstream interface sink")
    add("valid-alias-chain", """
type A: S; type B: A;
fn reflect(stack B value): A { ret value; }
fn take(stack A value): i32 { ret value.n; }
fn main(void): i32 { stack S value=.{.n=41}; ret take(reflect(value))-41; }
""", route="argument+return", source_contract="ordinary aliases")
    add("valid-reversed-declarations", """
fn take(stack S value): i32 { ret value.n; }
fn make(void): S { stack let value=S {.n=41}; ret value; }
fn main(void): i32 { ret take(make())-41; }
""", header=REVERSED_HEADER, route="argument+return", source_contract="inferred identifier and named-call result")
    add("valid-transitive-implementer", """
type J: interface.[I] {};
type Child: struct.[J] { ext i32 n; ext fn get(void): i32 { ret this.n; } };
fn reflect(stack Child value): Child { ret value; }
fn use(stack I value): i32 { ret value.get(); }
fn main(void): i32 { stack Child value=.{.n=41}; stack Child copy=reflect(value); ret use(copy)-41; }
""", route="argument+return", boundary="validated inherited interface membership")
    add("valid-scalar-conversions", """
fn real(stack f32 value): f32 { ret value; }
fn wide(stack i64 value): i64 { ret value; }
fn unsigned(stack u32 value): u32 { ret value; }
fn main(void): i32 {
  stack i8 small=41; stack u8 byte=41; stack i64 large=41;
  if(real(41)!=41.0 || wide(small)!=41 || unsigned(byte)!=41){ret 1;}
  ret (i32)large-41;
}
""", source_contract="primitive literal, widening, signedness and explicit cast", boundary="primitive destinations excluded")
    add("valid-same-generic", """
fn reflect(stack G.[i32] value): G.[i32] { ret value; }
fn take(stack G.[i32] value): i32 { ret value.n; }
fn @comptime[U] identity(stack U value): U { ret value; }
fn main(void): i32 { stack G.[i32] value=.{.n=41}; ret identity.[i32](take(reflect(value)))-41; }
""", source_contract="same generic application", boundary="generic destinations and generic calls excluded")
    add("valid-literal-generic-spelling", """
fn reflect(stack G_G_i32 value): G_G_i32 { ret value; }
fn take(stack G_G_i32 value): i32 { ret value.n; }
fn main(void): i32 { stack G_G_i32 value=.{.n=41}; ret take(reflect(value))-41; }
""", source_contract="ordinary declaration whose spelling contains _G_", boundary="spelling is not origin")
    add("valid-generic-ordinary-result", """
fn @comptime[U] make(stack U unused): S { stack S value=.{.n=41}; ret value; }
fn take(stack S value): i32 { ret value.n; }
fn main(void): i32 { ret take(make.[i32](1))-41; }
""", source_contract="generic producer with ordinary declared result", boundary="no body certification")
    add("valid-holder-factory-without-projection", """
type Holder: struct { ext S value; ext i32 flag; };
fn make(void): Holder { stack Holder h=.{.value=.{.n=41},.flag=7}; ret h; }
fn inspect(stack Holder h): i32 { ret h.value.n+h.flag; }
fn main(void): i32 { ret inspect(make())-48; }
""", source_contract="Holder", boundary="containing aggregates do not recursively enter destination set")
    add("valid-nonimplementing-aggregate", """
fn reflect(stack T value): T { ret value; }
fn main(void): i32 { stack T value=.{.n=41}; stack T copy=reflect(value); ret copy.n-41; }
""", source_contract="nonimplementing ordinary aggregate", boundary="destination excluded")
    add("valid-c-aggregate", """
type Pair: struct { ext i32 x; ext i32 y; };
@[[abi: c]] ext fn c_reflect(stack Pair value): Pair { value.x+=3; ret value; }
fn main(void): i32 { stack Pair value=.{.x=17,.y=21}; stack Pair result=c_reflect(value); ret result.x+result.y-41; }
""", route="C ABI", source_contract="ordinary nonimplementing C aggregate", boundary="C adapters and bodies excluded")
    add("valid-callback", """
fn reflect(stack S value): S { ret value; }
fn main(void): i32 { stack fn*(stack S): S callback=reflect; stack S value=.{.n=41}; stack S result=callback(value); ret result.n-41; }
""", route="callback", boundary="indirect argument route excluded")
    add("valid-methods", """
type Methods: struct {
  ext i32 offset;
  ext fn reflect(stack S value): S { ret value; }
  ext fn take(stack S value): i32 { ret value.n+this.offset; }
};
fn main(void): i32 { stack Methods methods=.{.offset=1}; stack S value=.{.n=40}; stack S copy=methods.reflect(value); ret methods.take(copy)-41; }
""", route="instance method", boundary="receiver adapters and instance return bodies excluded")
    add("valid-static-adapter", """
type Helper: struct { ext i32 n; };
fn Helper.reflect(stack S value): S { ret value; }
fn main(void): i32 { stack S value=.{.n=41}; stack S copy=Helper.reflect(value); ret copy.n-41; }
""", route="static adapter", boundary="call adapter excluded; exact receiver-free return body remains ordinary")
    add("valid-varargs", """
fn sum(stack i32 count,...): i32 { stack i32 total=0; stack va args; va.foreach.[i32](count,args) {|value| total+=value;}; ret total; }
fn main(void): i32 { ret sum(2,17,24)-41; }
""", route="variadic", source_contract="primitive varargs", boundary="variadic transport excluded")
    add("valid-async", """
async fn work(stack i32 value): i32 { ret value+1; }
fn main(void): i32 { ret await.(work)(40)-41; }
""", route="async", source_contract="primitive coroutine", boundary="promise returns and async adapters excluded")
    add("valid-thread", """
fn work(stack i32 value): i32 { ret value+1; }
fn main(void): i32 { stack future f=thread.(work)(40); stack i32 result=future.get.(i32)(f); future.drop.(f); ret result-41; }
""", route="thread", source_contract="primitive thread worker", boundary="thread adapter excluded")
    add("valid-lambda", """
fn main(void): i32 { stack fn*(stack i32): i32 f=lam.(stack i32 value): i32 { ret value+1; }; ret f(40)-41; }
""", route="lambda", boundary="generated lambda body excluded")
    add("valid-multi-results", """
fn pair(void): [S,i32] { stack S value=.{.n=17}; ret value,24; }
fn main(void): i32 { stack let [value,n]=pair(); ret value.n+n-41; }
""", route="multi-result", boundary="multiple returns and extraction excluded")
    add("valid-anonymous-construction", """
fn take(stack S value): i32 { ret value.n; }
fn make(void): S { ret .{.n=41}; }
fn main(void): i32 { stack S value=.{.n=41}; if(take(value)!=41){ret 1;} ret take(make())-41; }
""", route="argument+return", source_contract="anonymous contextual construction", boundary="NoImmediateEvidence; existing construction rules")
    add("valid-evaluation-once-order", """
stack i32 events=0;
fn make(stack i32 n): S { events=events*10+n; ret S {.n=n+19}; }
fn pair(stack S first,stack S second): i32 { ret first.n+second.n; }
fn main(void): i32 { stack i32 result=pair(make(1),make(2)); if(events!=12){ret 1;} ret result-41; }
""", route="argument+return", source_contract="effectful named-call result", boundary="source queries emit no IR and evaluate no operand")
    add("valid-return-before-cleanup", """
stack i32 events=0;
fn mark(void): S { events=events*10+1; ret S {.n=41}; }
fn make(void): S { defer events=events*10+2; ret mark(); }
fn main(void): i32 { stack S result=make(); if(events!=12){ret 1;} ret result.n-41; }
""", route="return", source_contract="effectful named-call result", boundary="return operand evaluates once before cleanup")
    add("valid-nested-generic-lambda-context", """
fn @comptime[U] identity(stack U value): U { ret value; }
fn make(void): S {
  stack i32 n=identity.[i32](40);
  stack fn*(stack i32): i32 f=lam.(stack i32 value): i32 { ret value+1; };
  stack S value=.{.n=f(n)}; ret value;
}
fn main(void): i32 { stack S value=make(); ret value.n-41; }
""", route="return", boundary="ordinary body identity restored after nested generation")
    add("valid-generic-argument-preparation", """
fn @comptime[U] identity(stack U value): U { ret value; }
fn take(stack i32 adjustment, stack S value): i32 { ret value.n+adjustment; }
fn main(void): i32 { stack S value=.{.n=40}; ret take(identity.[i32](1),value)-41; }
""", route="argument", boundary="selected named-function metadata survives generic argument preparation")

    # Required rejection family: raw original nominal identities only.
    families = (
        ("ordinary", "T", "S"),
        ("generic", "G.[i32]", "S"),
        ("generic-equal-native", "G.[i32]", "G_G_i32"),
    )
    for family, actual, destination in families:
        forms = (
            ("typed-identifier", f"stack {actual} wrong=.{{.n=7}};", "wrong", ""),
            ("inferred-identifier", f"stack let wrong={actual} {{.n=7}};", "wrong", ""),
            ("named-construction", "", f"{actual} {{.n=7}}", ""),
            ("named-call-result", "", "produce()", f"fn produce(void): {actual} {{ stack {actual} wrong=.{{.n=7}}; ret wrong; }}\n"),
        )
        for form, setup, operand, producer in forms:
            add(f"reject-argument-{family}-{form}", producer + f"""
fn take(stack {destination} value): i32 {{ ret value.n; }}
fn main(void): i32 {{ {setup}
  ret /*@seam*/take(/*@operand*/{operand});
}}
""", "reject", route="argument", source_contract=family+" "+form,
                boundary="immediate mismatch; no downstream interface sink")
            add(f"reject-return-{family}-{form}", producer + f"""
fn wrong_result(void): {destination} {{ {setup}
  /*@seam*/ret /*@operand*/{operand};
}}
fn main(void): i32 {{ ret 0; }}
""", "reject", route="return", source_contract=family+" "+form,
                boundary="emitted explicit return checked even when function is never called")

    for route in ("argument", "return"):
        for family, actual, destination in (families[0], families[2]):
            for variation in ("aliases", "reversed", "downstream-sink", "internal", "public"):
                alias = variation == "aliases"
                prefix = "int " if variation == "internal" else "ext " if variation == "public" else ""
                declarations = f"type Wrong: {actual}; type Target: {destination};\n" if alias else ""
                src, dst = ("Wrong", "Target") if alias else (actual, destination)
                sink = variation == "downstream-sink"
                if sink:
                    declarations += "fn use(stack I value): i32 { ret value.get(); }\n"
                if route == "argument":
                    body = declarations + f"""
{prefix}fn take(stack {dst} value): i32 {{ ret {'use(value)' if sink else 'value.n'}; }}
fn main(void): i32 {{ stack {src} wrong=.{{.n=7}};
  ret /*@seam*/take(/*@operand*/wrong);
}}
"""
                else:
                    body = declarations + f"""
{prefix}fn wrong_result(void): {dst} {{ stack {src} wrong=.{{.n=7}};
  /*@seam*/ret /*@operand*/wrong;
}}
fn main(void): i32 {{ {'stack '+dst+' value=wrong_result(); ret use(value);' if sink else 'ret 0;'} }}
"""
                add(f"reject-{route}-{family}-{variation}", body, "reject", route=route,
                    source_contract=family+" typed identifier", boundary=variation,
                    header=REVERSED_HEADER if variation == "reversed" else HEADER)

    for wrong_first in (False, True):
        for nested in (False, True):
            good = "ret good;"
            bad = "/*@seam*/ret /*@operand*/wrong;"
            first, second = (bad, good) if wrong_first else (good, bad)
            inner = f"if(flag){{ {first} }} else {{ {second} }}"
            if nested:
                inner = "{ { " + inner + " } }"
            add(f"reject-return-branches-{'wrong-first' if wrong_first else 'wrong-second'}-{'nested' if nested else 'flat'}", f"""
fn wrong_result(stack bool flag): S {{ stack S good=.{{.n=41}}; stack T wrong=.{{.n=7}};
  {inner}
}}
fn main(void): i32 {{ ret 0; }}
""", "reject", route="return", source_contract="ordinary typed identifier",
                boundary="each emitted original explicit return; independent of source order")
    add("reject-argument-parameter", """
fn take(stack S value): i32 { ret value.n; }
fn relay(stack T wrong): i32 {
  ret /*@seam*/take(/*@operand*/wrong);
}
fn main(void): i32 { ret 0; }
""", "reject", route="argument", source_contract="typed formal identifier", boundary="raw immediate declaration; no ingress theorem")
    add("reject-return-parameter", """
fn relay(stack T wrong): S {
  /*@seam*/ret /*@operand*/wrong;
}
fn main(void): i32 { ret 0; }
""", "reject", route="return", source_contract="typed formal identifier", boundary="raw immediate declaration; no ingress theorem")
    add("reject-shadowed-local-selection", """
fn take(stack S value): i32 { ret value.n; }
fn main(void): i32 { stack S value=.{.n=41}; if(value.n!=41){ret 1;} { stack T value=.{.n=7};
  ret /*@seam*/take(/*@operand*/value);
} }
""", "reject", route="argument", source_contract="nearest typed identifier", boundary="do not select an outer same-name declaration")
    add("reject-return-after-generic-lambda-preparation", """
fn @comptime[U] identity(stack U value): U { ret value; }
fn wrong_result(void): S {
  stack i32 n=identity.[i32](7);
  stack fn*(stack i32): i32 f=lam.(stack i32 value): i32 { ret value; };
  stack T wrong=.{.n=f(n)};
  /*@seam*/ret /*@operand*/wrong;
}
fn main(void): i32 { ret 0; }
""", "reject", route="return", source_contract="ordinary typed identifier", boundary="current original ordinary body restored after nested generated bodies")
    add("reject-argument-after-generic-preparation", """
fn @comptime[U] identity(stack U value): U { ret value; }
fn take(stack i32 adjustment, stack S value): i32 { ret value.n+adjustment; }
fn main(void): i32 { stack T wrong=.{.n=7};
  ret /*@seam*/take(identity.[i32](1),/*@operand*/wrong);
}
""", "reject", route="argument", source_contract="ordinary typed identifier", boundary="all immediate comparisons precede generic argument preparation")
    add("reject-bodyless-formal-contract", """
ext fn take(stack G_G_i32 value): i32;
fn main(void): i32 { stack G.[i32] wrong=.{.n=7};
  ret /*@seam*/take(/*@operand*/wrong);
}
""", "reject", route="argument", source_contract="generic typed identifier", boundary="complete native declaration supplies immediate formal without body proof")
    add("reject-bodyless-result-contract", """
ext fn produce(void): G.[i32];
fn take(stack G_G_i32 value): i32 { ret value.n; }
fn main(void): i32 {
  ret /*@seam*/take(/*@operand*/produce());
}
""", "reject", route="argument", source_contract="generic named-call result", boundary="declared result suffices; producer body unavailable and not inspected")
    add("reject-global-original-contract", """
stack T wrong;
fn take(stack S value): i32 { ret value.n; }
fn main(void): i32 {
  ret /*@seam*/take(/*@operand*/wrong);
}
""", "reject", route="argument", source_contract="ordinary typed global identifier", boundary="original global declaration only; no initialization theorem")
    add("reject-transitive-implementer-target", """
type J: interface.[I] {};
type Child: struct.[J] { ext i32 n; ext fn get(void): i32 { ret this.n; } };
fn take(stack Child value): i32 { ret value.n; }
fn main(void): i32 { stack T wrong=.{.n=7};
  ret /*@seam*/take(/*@operand*/wrong);
}
""", "reject", route="argument", source_contract="ordinary typed identifier", boundary="target explicitly implements inherited interface graph")
    add("reject-imported-named-constructor-return", "fn main(void): i32 { ret 0; }",
        "reject", route="return", source_contract="ordinary named-construction",
        boundary="fallback span belongs to the original imported function source",
        header="mod immediate;\nimp wrong;\n", expected_diagnostic_file="wrong.sts",
        files={"wrong.sts": """mod wrong;
ext type I: interface { get(void): i32; };
ext type S: struct.[I] { ext i32 n; ext fn get(void): i32 { ret this.n; } };
ext type T: struct { ext i32 n; };
ext fn wrong(void): S {
  /*@seam*/ret /*@operand*/T {.n=7};
}
"""})

    # Explicitly accepted-but-unclosed stored history: NEVER EXECUTE.
    for family, actual, destination in families:
        for route in ("argument", "return"):
            for writer in ("initializer", "assignment"):
                setup = f"stack {actual} wrong=.{{.n=7}};\n"
                setup += (f"stack {destination} declared=wrong;" if writer == "initializer" else
                          f"stack {destination} declared=.{{.n=41}}; declared=wrong;")
                if route == "argument":
                    body = f"fn take(stack {destination} value): i32 {{ ret value.n; }}\nfn main(void): i32 {{ {setup} ret take(declared); }}"
                else:
                    body = f"fn make(void): {destination} {{ {setup} ret declared; }}\nfn main(void): i32 {{ ret 0; }}"
                add(f"deferred-stored-history-{family}-{route}-{writer}", body, "deferred",
                    route=route, source_contract="matching ordinary annotation with prior "+family+" writer",
                    boundary="unclosed stored-history boundary; inherited generic-origin bit is not immediate mismatch")

    # Unsupported raw expression forms. The collision makes historical native
    # acceptance observable without a verifier failure obscuring the boundary.
    expression_forms = (
        ("field", "type Box: struct { ext G.[i32] value; };", "stack Box box=.{.value=.{.n=7}};", "box.value"),
        ("index", "", "stack G.[i32] values[1]=.{G.[i32] {.n=7}};", "values[0]"),
        ("dereference", "", "stack G.[i32] wrong=.{.n=7}; stack G.[i32] *rw p=&wrong;", "*p"),
        ("conditional", "", "stack G.[i32] wrong=.{.n=7};", "true?wrong:wrong"),
        ("assignment", "", "stack G.[i32] wrong=.{.n=7}; stack G.[i32] copy=.{.n=8};", "(copy=wrong)"),
        ("cast-dereference", "", "stack G.[i32] wrong=.{.n=7};", "*(G_G_i32 *r)&wrong"),
        ("callback-result", "fn produce(void): G.[i32] { stack G.[i32] value=.{.n=7}; ret value; }", "stack fn*(void): G.[i32] callback=produce;", "callback()"),
    )
    for form, declarations, setup, operand in expression_forms:
        for route in ("argument", "return"):
            if route == "argument":
                body = declarations + f"\nfn take(stack G_G_i32 value): i32 {{ ret value.n; }}\nfn main(void): i32 {{ {setup} ret take({operand}); }}"
            else:
                body = declarations + f"\nfn make(void): G_G_i32 {{ {setup} ret {operand}; }}\nfn main(void): i32 {{ ret 0; }}"
            add(f"deferred-expression-{form}-{route}", body, "deferred", route=route,
                source_contract=form, boundary="unsupported immediate expression; no recursive producer/history traversal")

    add("deferred-holder-target", """
type Holder: struct { ext S value; };
type WrongHolder: struct { ext T value; };
fn take(stack Holder value): i32 { ret value.value.n; }
fn main(void): i32 { stack WrongHolder wrong=.{.value=.{.n=7}}; stack Holder declared=wrong; ret take(declared); }
""", "deferred", route="argument", source_contract="Holder with wrong prior child", boundary="Holder is not an affected destination")
    add("deferred-indirect-argument", """
fn take(stack G_G_i32 value): i32 { ret value.n; }
fn main(void): i32 { stack fn*(stack G_G_i32): i32 callback=take; stack G.[i32] wrong=.{.n=7}; ret callback(wrong); }
""", "deferred", route="callback", source_contract="immediate generic actual", boundary="function-pointer load excluded from ordinary named route")
    add("deferred-shadowed-function-value", """
fn take(stack G_G_i32 value): i32 { ret value.n; }
fn other(stack G_G_i32 value): i32 { ret value.n; }
fn main(void): i32 { stack fn*(stack G_G_i32): i32 take=other; stack G.[i32] wrong=.{.n=7}; ret take(wrong); }
""", "deferred", route="callback", source_contract="same-name function-pointer local", boundary="selected physical route wins over same-name top-level declaration")
    add("deferred-instance-adapter", """
type Helper: struct { ext fn take(stack G_G_i32 value): i32 { ret value.n; } };
fn main(void): i32 { stack Helper helper=.{}; stack G.[i32] wrong=.{.n=7}; ret helper.take(wrong); }
""", "deferred", route="instance adapter", source_contract="immediate generic actual", boundary="implicit receiver mapping excluded")
    add("deferred-static-adapter", """
type Helper: struct { ext i32 n; };
fn Helper.take(stack G_G_i32 value): i32 { ret value.n; }
fn main(void): i32 { stack G.[i32] wrong=.{.n=7}; ret Helper.take(wrong); }
""", "deferred", route="static-dot adapter", source_contract="immediate generic actual", boundary="static-dot call adapter excluded")
    add("deferred-constructor-adapter", """
type Helper: struct { ext i32 n; };
fn Helper.new(stack G_G_i32 value): Helper { stack Helper result=.{.n=value.n}; ret result; }
fn main(void): i32 { stack G.[i32] wrong=.{.n=7}; stack Helper result=Helper.(wrong); ret result.n; }
""", "deferred", route="constructor adapter", source_contract="immediate generic actual", boundary="constructor call adapter excluded")
    for name, call in (("self", "this.take(wrong)"), ("sibling", "take(wrong)")):
        add("deferred-" + name + "-adapter", f"""
type Helper: struct {{
  ext fn take(stack G_G_i32 value): i32 {{ ret value.n; }}
  ext fn relay(stack G.[i32] wrong): i32 {{ ret {call}; }}
}};
fn main(void): i32 {{ ret 0; }}
""", "deferred", route=name+" adapter", source_contract="immediate generic actual", boundary="implicit receiver adapter excluded")
    module = """mod api;
ext type MI: interface { get(void): i32; };
ext type Q: @comptime[U] struct.[MI] { ext U n; ext fn get(void): i32 { ret 7; } };
ext type Q_G_i32: struct.[MI] { ext i32 n; ext fn get(void): i32 { ret this.n; } };
ext fn take(stack Q_G_i32 value): i32 { ret value.n; }
"""
    for name, call in (("module-dot", "api.take"), ("module-colon", "api:take")):
        add("deferred-" + name + "-adapter", f"""
imp api;
fn main(void): i32 {{ stack Q.[i32] wrong=.{{.n=7}}; ret {call}(wrong); }}
""", "deferred", route=name+" adapter", source_contract="immediate generic actual", boundary="module adapter excluded", files={"api.sts": module})
    add("deferred-c-argument-adapter", """
@[[abi: c]] ext fn foreign(stack G_G_i32 value): i32;
fn main(void): i32 { stack G.[i32] wrong=.{.n=7}; ret foreign(wrong); }
""", "deferred", route="C ABI argument", source_contract="immediate generic actual", boundary="C ABI transport excluded")
    add("deferred-thread-argument-adapter", """
fn take(stack G_G_i32 value): i32 { ret value.n; }
fn main(void): i32 { stack G.[i32] wrong=.{.n=7}; stack future f=thread.(take)(wrong); future.drop.(f); ret 0; }
""", "deferred", route="thread adapter", source_contract="immediate generic actual", boundary="generated dispatch excluded")
    add("deferred-async-argument-adapter", """
async fn take(stack G_G_i32 value): i32 { ret value.n; }
fn main(void): i32 { stack G.[i32] wrong=.{.n=7}; ret await.(take)(wrong); }
""", "deferred", route="async adapter", source_contract="immediate generic actual", boundary="async call adapter excluded")
    add("deferred-instance-return", """
type Helper: struct { ext fn make(void): G_G_i32 { stack G.[i32] wrong=.{.n=7}; ret wrong; } };
fn main(void): i32 { ret 0; }
""", "deferred", route="instance return", source_contract="immediate generic operand", boundary="instance body excluded")
    add("deferred-generic-body-return", """
fn @comptime[U] make(stack U unused): G_G_i32 { stack G.[i32] wrong=.{.n=7}; ret wrong; }
fn main(void): i32 { stack G_G_i32 result=make.[i32](1); ret result.n; }
""", "deferred", route="generic body return", source_contract="immediate generic operand", boundary="instantiated generic body excluded")
    add("deferred-lambda-body-return", """
fn main(void): i32 {
  stack fn*(void): G_G_i32 callback=lam.(void): G_G_i32 { stack G.[i32] wrong=.{.n=7}; ret wrong; };
  stack G_G_i32 result=callback(); ret result.n;
}
""", "deferred", route="lambda return", source_contract="immediate generic operand", boundary="generated lambda body excluded")
    add("deferred-async-body-return", """
async fn make(void): G_G_i32 { stack G.[i32] wrong=.{.n=7}; ret wrong; }
fn main(void): i32 { ret 0; }
""", "deferred", route="async return", source_contract="immediate generic operand", boundary="coroutine promise return excluded")
    add("deferred-multi-result-return", """
fn make(void): [G_G_i32,i32] { stack G.[i32] wrong=.{.n=7}; ret wrong,1; }
fn main(void): i32 { ret 0; }
""", "deferred", route="multi-result return", source_contract="immediate generic operand", boundary="multiple result body and extraction excluded")
    add("deferred-missing-return", """
fn make(void): S { stack i32 _unused=7; }
fn main(void): i32 { ret 0; }
""", "deferred", route="fallthrough", source_contract="no explicit value", boundary="no initialization or return coverage theorem")
    add("deferred-bare-return", """
fn make(void): S { ret; }
fn main(void): i32 { ret 0; }
""", "deferred", route="bare return", source_contract="no explicit value", boundary="bare return excluded")
    add("deferred-partial-return", """
fn make(stack bool flag): S { if(flag){stack S value=.{.n=41}; ret value;} }
fn main(void): i32 { ret 0; }
""", "deferred", route="partial fallthrough", source_contract="matching explicit return with reachable fallthrough", boundary="only emitted explicit operands checked; no return coverage theorem")
    add("deferred-default-descriptor", """
fn make(void): I { stack I value; ret value; }
fn main(void): i32 { ret 0; }
""", "deferred", route="descriptor return", source_contract="default descriptor", boundary="descriptor authenticity and initialization are not established")

    assert len(cases) == len({case.name for case in cases}), "duplicate fixture name"
    assert all(case.execute == (case.category == "valid") for case in cases)
    assert all(case.expected_exit is None for case in cases if case.compile_only)
    return cases


def inventory(cases=None):
    """Human/machine-readable scope without importing or changing old oracles."""
    return [dict(name=case.name, category=case.category, route=case.route,
                 source_contract=case.source_contract, boundary=case.boundary,
                 execute=case.execute, optimization_levels=list(LEVELS),
                 diagnostic=case.diagnostic, operand_span=case.expected_span,
                 seam_span=case.seam_span,
                 diagnostic_span=case.expected_diagnostic_span,
                 diagnostic_file=case.expected_diagnostic_file,
                 expected_exit=case.expected_exit, expected_stdout=case.expected_stdout,
                 expected_stderr=case.expected_stderr,
                 source_sha256=hashlib.sha256(case.source.encode()).hexdigest(),
                 file_sha256={name: hashlib.sha256(text.encode()).hexdigest()
                              for name, text in sorted(case.files.items())})
            for case in (matrix() if cases is None else cases)]
