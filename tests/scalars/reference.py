"""Independent arbitrary-precision oracle for Stasha's P1 scalar contract.

Python integers are unbounded. Truncation, signed interpretation, division toward
zero, and overflow detection are explicit; no host C/LLVM arithmetic is used.
"""
from dataclasses import dataclass


class ScalarError(ValueError):
    """An ill-typed source expression."""


class ScalarTrap(ArithmeticError):
    """A defined arithmetic exception, with a stable oracle reason."""


@dataclass(frozen=True)
class IntType:
    width: int
    signed: bool

    def __post_init__(self):
        if self.width not in (8, 16, 32, 64):
            raise ScalarError("unsupported width")

    @property
    def name(self):
        return ("i" if self.signed else "u") + str(self.width)

    @property
    def minimum(self):
        return -(1 << (self.width - 1)) if self.signed else 0

    @property
    def maximum(self):
        return (1 << (self.width - int(self.signed))) - 1

    @property
    def mask(self):
        return (1 << self.width) - 1

    def fits(self, value):
        return self.minimum <= value <= self.maximum

    def wrap(self, value):
        bits = value & self.mask
        return bits - (1 << self.width) if self.signed and bits >> (self.width - 1) else bits


TYPES = tuple(IntType(w, s) for w in (8, 16, 32, 64) for s in (True, False))
BY_NAME = {t.name: t for t in TYPES}


def literal(value, context=None):
    """value includes a directly applied unary minus, including signed MIN."""
    if context is not None:
        if not context.fits(value):
            raise ScalarError("literal out of range for " + context.name)
        return value, context
    for name in ("i32", "i64", "u64"):
        typ = BY_NAME[name]
        if typ.fits(value):
            return value, typ
    raise ScalarError("literal out of range")


def common_type(left, right):
    if left.signed != right.signed:
        raise ScalarError("mixed signedness requires an explicit cast")
    return IntType(max(left.width, right.width), left.signed)


def cast(value, source, destination):
    # Normalizing at the SOURCE width first is crucial: u8(255) -> u64(255),
    # while i8(-1) -> u64(18446744073709551615).
    return destination.wrap(source.wrap(value))


def unary(op, value, typ):
    value = typ.wrap(value)
    if op == "-":
        return typ.wrap(-value)
    if op == "~":
        return typ.wrap(~value)
    if op == "!":
        return not value
    raise ScalarError("unknown unary operator " + op)


def binary(op, left, right, typ):
    left = typ.wrap(left)
    # A shift's right operand retains its own count value and is not coerced
    # to the left width. In particular, 256 <<count on u8 must not become 0.
    if op in ("<<", ">>"):
        if not 0 <= right < typ.width:
            raise ScalarTrap("invalid-shift")
        return typ.wrap(left << right) if op == "<<" else left >> right
    right = typ.wrap(right)
    if op in ("/", "%"):
        if right == 0:
            raise ScalarTrap("division-by-zero")
        if typ.signed and left == typ.minimum and right == -1:
            raise ScalarTrap("division-overflow")
        quotient = abs(left) // abs(right)
        if (left < 0) != (right < 0):
            quotient = -quotient
        return quotient if op == "/" else left - quotient * right
    if op in ("==", "!=", "<", "<=", ">", ">="):
        return {"==": left == right, "!=": left != right, "<": left < right,
                "<=": left <= right, ">": left > right, ">=": left >= right}[op]
    if op in ("&", "|", "^"):
        return typ.wrap({"&": left & right, "|": left | right, "^": left ^ right}[op])
    if op in ("+", "-", "*", "+%", "-%", "*%", "+!", "-!", "*!"):
        result = {"+": left + right, "-": left - right, "*": left * right}[op[0]]
        if op.endswith("!") and not typ.fits(result):
            raise ScalarTrap("overflow")
        return typ.wrap(result)
    raise ScalarError("unknown binary operator " + op)


def logical(op, left, right):
    """Both operands are thunks so tests can prove the inactive side is skipped."""
    if op == "&&":
        return bool(left()) and bool(right())
    if op == "||":
        return bool(left()) or bool(right())
    raise ScalarError("unknown logical operator " + op)


def boundaries(typ):
    values = [0, 1, typ.maximum - 1, typ.maximum]
    values += [typ.minimum, typ.minimum + 1, -1] if typ.signed else [1 << (typ.width - 1)]
    return tuple(dict.fromkeys(values))


def canonical_hex(value, typ):
    """The generated executable emits every integer after an explicit u64 cast."""
    return format(cast(value, typ, BY_NAME["u64"]), "016x")
