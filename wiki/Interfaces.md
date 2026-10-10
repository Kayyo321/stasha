# Interfaces

Interfaces declare a set of methods. Concrete calls can resolve statically; interface-typed calls use a fat pointer and an indirect vtable call. The current compiler does not yet verify the complete source signature and implementation contract. See the bounded I0 status below.

---

## Declaring an Interface

```stasha
type Drawable: interface {
    draw(void): void;
}
```

- Method signatures end with `;` (no body)
- Parameter names are optional in signatures

```stasha
type Entity: interface {
    id(void): i32;
    name(void): i8 *r;
}
```

---

## Interface Inheritance

Interfaces can extend other interfaces:

```stasha
type Entity: interface {
    id(void): i32;
}

type Movable: interface.[Entity] {
    move(i32 x, y): void;
}

type Attackable: interface.[Entity] {
    attack(Entity target): void;
}

// Combine multiple:
type Character: interface.[Movable, Attackable] {
    get_health(void): i32;
    set_health(i32 hp): void;
}
```

Implementing `Character` requires implementing all methods from `Movable`, `Attackable`, `Entity`, and `Character` itself.

---

## Implementing an Interface

Use `struct.[InterfaceName, ...]` to declare that a struct implements interfaces:

```stasha
type Player: struct.[Character, Renderable] {
    int i32 player_id;
    int i32 player_health;
    int i32 player_x, player_y;

    ext fn id(void): i32 => this.player_id;
    ext fn get_health(void): i32 => this.player_health;
    ext fn set_health(i32 new_health): void {
        this.player_health = new_health;
    }

    ext fn move(i32 x, y): void {
        this.player_x += x;
        this.player_y += y;
        print.('Player {} moved to ({},{})\n', this.id(), this.player_x, this.player_y);
    }

    ext fn attack(Entity target): void {
        print.('Player {} attacks entity {}\n', this.id(), target.id());
    }

    ext fn render(void): void {
        print.('Rendering player {}\n', this.id());
    }
}

fn Player.new(i32 id, health, x, y): Player {
    ret Player {
        .player_id     = id,
        .player_health = health,
        .player_x      = x,
        .player_y      = y,
    };
}
```

Complete missing-method and signature rejection is pending the I1 implementation. Until that stage is independently qualified, do not rely on an interface declaration as proof that every table entry is valid.

---

## Calling Interface Methods

Methods are called through the concrete type normally:

```stasha
Player p = Player.(1, 100, 0, 0);
p.move(5, 7);
p.attack(p);   // attack self for demo
p.render();
```

When passed as an interface type, dynamic dispatch happens:

```stasha
fn render_all(stack []Entity entities): void {
    foreach e in entities {
        e.render();   // dispatched through interface
    }
}
```

---

## Method Ambiguity Resolution

When a struct implements two interfaces that both define a method with the same name, use qualified method names to disambiguate:

```stasha
type Flyable: interface {
    move(void): void;
}

type Swimmable: interface {
    move(void): void;
}

type Duck: struct.[Flyable, Swimmable] {
    ext fn flyable.move(void): void {
        print.('Duck flies\n');
    }

    ext fn swimmable.move(void): void {
        print.('Duck swims\n');
    }
}

Duck d = Duck.();
d.flyable.move();    // "Duck flies"
d.swimmable.move();  // "Duck swims"
```

---

## Constrained Generics

Use `T.[InterfaceName]` in a generic to require that `T` implements an interface:

```stasha
type EntityHolder: @comptime[T.[Entity]] struct {
    int stack T *r entity;

    ext fn print_id(void): void {
        print.('Entity id: {}\n', this.entity.id());
    }
}

fn @comptime[T.[Entity]] EntityHolder.new(stack T *r ent): EntityHolder.[T] {
    ret EntityHolder { .entity = ent };
}
```

This ensures at compile time that `T` has an `id()` method. Instantiating with a type that doesn't implement `Entity` is a compile error.

Usage:
```stasha
Player p = Player.(1, 100, 0, 0);
EntityHolder.[Player] holder = EntityHolder.[Player].new(&p);
holder.print_id();   // "Entity id: 1"
```

---

## Interface as Parameter Type

Pass an interface as a parameter to get polymorphic behavior:

```stasha
fn log_entity(Entity e): void {
    print.('Entity id={}\n', e.id());
}

Player p = Player.(42, 100, 0, 0);
log_entity(p);   // Player passed as Entity
```

---

## Minimal Interface Example

A complete minimal example:

```stasha
type Greetable: interface {
    greet(void): void;
}

type Human: struct.[Greetable] {
    ext i8 *r name;

    ext fn greet(void): void {
        print.('Hello, I am {}\n', this.name);
    }
}

type Robot: struct.[Greetable] {
    ext i32 id;

    ext fn greet(void): void {
        print.('BEEP BOOP, I am Robot-{}\n', this.id);
    }
}

fn introduce(Greetable g): void {
    g.greet();
}

Human alice = .{ .name = "Alice" };
Robot r2d2  = .{ .id   = 2       };

introduce(alice);   // "Hello, I am Alice"
introduce(r2d2);    // "BEEP BOOP, I am Robot-2"
```

---

## Common Mistakes

**Missing a required method:**
```stasha
type Foo: struct.[Drawable] {
    // forgot to implement draw() — compile error
}
```

**Using `this` in a static method:**
```stasha
fn Foo.new(): Foo {
    this.x = 0;   // ERROR: this is not available in static methods
}
```

**Wrong method signature:**
```stasha
type Drawable: interface { draw(void): void; }

type Foo: struct.[Drawable] {
    ext fn draw(i32 x): void { ... }  // ERROR: signature doesn't match interface
}
```

## I0 graph and metadata checkpoint

The graph rules below remain active. The I1a section that follows supersedes the
historical unresolved-signature restriction and ABI 6 build requirement.

I0 validates the selected declaration graph before type lowering. It rejects
unknown or non-interface parents, duplicate declarations/parents/own methods,
cycles, and unsupported interface metadata with source diagnostics. Limits are
8 direct parents or implementations, 32 own methods, 32 unique flattened method
names, and 64 interfaces on an inheritance path. Exceeding a limit is an error;
lists are not silently truncated. Interface metadata traversal is bounded at
256 levels, with alias-cycle diagnostics. Existing fixed representations also
now diagnose more than 8 pointer levels, more than 32 callback parameters, or
formatted generic/interface names exceeding 511 bytes instead of losing their
contract. Previously truncated inputs need to be simplified or split.

Inherited slots follow parent declaration order, recursively, then own methods.
A repeated path to the same originating method declaration contributes one slot.
Distinct declarations with the same method name remain separate pending origins,
even if their signatures appear equal. At the I0 checkpoint, consuming such a group for a table or
interface call was rejected pending canonical signature comparison; I1a below
resolves supported equal signatures and rejects conflicts. Empty-interface table construction is also temporarily unsupported.
Generic interfaces, interface return-storage annotations, and generic interface
constraints are diagnosed rather than interpreted as verified contracts.

### Archive migration: ABI 6

The cached graph removes duplicate ancestor slots that the older collector could
emit for diamond inheritance. This changes interface dispatch slot ordering.
ABI 6 therefore rejects ABI 5 and older Stasha archives. Rebuild the compiler,
all Stasha libraries, and every consumer together. Do not edit archive sidecar
metadata to bypass the check. The two-word object/table descriptor layout itself
is unchanged; its slot interpretation is incompatible.

For example, if D inherits B and C, and both inherit A, a method originating in
A occupies one D slot. Rebuild both the library that creates D interface values
and every executable or library that calls through them.

I0 is only graph validation and source metadata retention. Full parameter,
receiver, return, ownership-effect and calling-convention agreement; rejection of
missing implementations; all boxing routes; protected table membership; indirect
call CFI; object lifetime and object/table pairing are separate pending gates.
This checkpoint provides no general interface memory-safety guarantee.

## I1a declared signatures and table binding

These binding rules remain active. I1b.1 below adds source-call checks and
supersedes the ABI7 build requirement.

I1a checks declared interface contracts before emitting native declarations or
tables. Same-name inherited requirements can share a slot only when their full
supported source signatures agree. Signedness, nominal enum/struct/interface
identity, parameter domains, every pointer permission layer and represented
outer nullability are significant. Alias names may differ when their supported
expanded contracts are equal; unsupported inner-nullable alias compositions
produce a diagnostic. Nullable pointer aliases can carry the marker in their
alias definition; the existing parser does not admit a use-site `?P` when P hides
all pointer stars.

A concrete type must explicitly declare the interface, directly or through its
inherited interface closure. Every required method must have one available,
compatible instance implementation. Selection chooses the declaration first:
exact interface-qualified method, then ordinary unqualified method, then one
unique related qualified implementation. A mismatched higher-priority method
is an error, not a reason to silently select a lower-priority match. Inline
owner-self spelling `fn S.m` remains an ordinary instance method; top-level
`fn S.m` is static and cannot supply the implicit receiver.

The bounded native subset includes fixed-arity ordinary instance methods with
scalar, plain-enum, supported data-pointer or exact interface-descriptor values.
Unsupported aggregate, callback/closure, C-ABI, variadic, coroutine and
caller-effect contracts diagnose explicitly in the checked relations. Generic
interface integration remains unqualified: an unused generic template can remain
accepted without its relations being verified. At the admitted concrete boxing
and qualified-call boundaries, retained generic or dependent source provenance
is excluded even when a generated LLVM name happens to match an ordinary type.
Unknown inferred concrete identity also requires a diagnostic at those boundaries.
Field/index projections are not yet qualified sources for concrete boxing: copying
one into a typed local does not erase its unresolved eligibility.
This bounded eligibility check is not generic instantiation or assignment-flow
verification. Ordinary concrete values inside unrelated generic functions, and
generic calls whose declared result is a known ordinary type, remain eligible.
Unrelated ordinary language features remain available. The method receiver is mutable; no readonly receiver
contract or body-based effect inference is introduced.

A table is published only after every selected declaration agrees with its
source contract and actual native function ABI. Missing methods never become
null entries. Dynamic and qualified interface calls use the formal cached
function type and convention, with arity/representation checks. This does not
yet provide complete source-type checking of every argument expression or
conversion route.

Existing readable identifier-valued concrete arguments can be shallow-copied
into a temporary for boxing when their declared relation is ready. The copy
retains field pointer permissions. Pointer-to-object boxing and implicit
pointer-to-interface receivers are not admitted by this stage. Object lifetime,
all expression forms and ownership remain separate work.

### Archive migration: ABI 7

I1a requires rebuilt Stasha archives and consumers for ABI 7. Unlike I0's ABI 6
slot-order change, this is a semantic archive compatibility marker: the table
slot ordering and two-word descriptor layout do not change again. Older bodies
can contain unchecked table construction, and source-backed imports do not
regenerate those bodies. Reading declarations alone cannot establish the new
binding guarantee. Do not relabel sidecars; rebuild the provider and consumer.

### Remaining interface gates

I1a certifies the bounded declared binding/native-entry contract, not general
interface memory safety. Full source-argument checks, all call/boxing routes,
casts, conditional merges, return/store/field/array/slice propagation and default
or partial descriptor initialization remain pending. Runtime table membership,
indirect-call CFI, object/table pairing, lifetime, ownership and concurrency are
separate requirements. An exact native call type does not prove that a descriptor
contains a live or authentic object and table.

## I1b.1 source contracts at interface calls

Dynamic and qualified interface-slot calls check every actual expression's
supported source type against its declared parameter. Supported native call
adapters check their interface-valued formals; unrelated native formals keep
the existing language checks. Admission precedes argument evaluation. Equal LLVM types or
pointer sizes alone do not establish source agreement. Source queries preserve
nominal declarations, aliases, pointer layers, nullability and relevant storage
requirements; unsupported or unresolved contracts diagnose instead of falling
back to native representation. The selected source callable must agree with the
native callable that will actually be emitted.

The native adapters cover existing general/function-pointer, sibling, instance,
self/self-member, module/static/colon and constructor calls with retained source
contracts. This does not add new concrete boxing on those adapters. Direct
interface descriptor carriers on unsupported lambda/closure/thread/async, C-ABI
or variadic routes diagnose rather than relying on their physical layout.

The bounded conversion rules are:

- Primitive arguments retain the established checked scalar conversions. Enum
  arguments require the same enum declaration, including aliases; their integer
  representation does not grant implicit conversion to a different enum.
- Data pointers require the same pointee declaration/type and depth. Inner
  permission layers remain invariant. Outer read/write/arithmetic capabilities
  may be removed, never added by an ordinary pointer conversion. A nonnullable
  pointer may pass to a nullable formal; the reverse is rejected.
- Direct addresses of supported ordinary object/array/field storage retain the
  established fresh-address capabilities, including arithmetic permission.
  Readonly storage cannot supply write permission. Pointer-derived addresses
  cannot add capabilities absent from their source pointer. This does not prove an extent or
  lifetime beyond the existing access rules.
- An explicitly required stack/heap pointee domain needs supported evidence
  about the selected object/pointee. The location of a pointer variable is not
  evidence of where it points. Unsupported inner-domain alias compositions
  diagnose rather than silently attaching the domain to the wrong layer.
- Literal nil has a dedicated conversion only to a nullable data-pointer formal
  without a stack/heap pointee-domain requirement. It cannot supply a nonnullable
  pointer or an interface descriptor. Known-domain nil remains unsupported.
- A readable value of exactly interface I may be copied into a default/stack I
  formal, including a declared I call result. This copies a descriptor and does
  not infer the domain, ownership or lifetime of its backing object. Interface
  J does not become I merely because both descriptors contain two pointers.

Existing readable identifier-valued concrete boxing remains bounded by checked
eligibility rules. No new implicit pointer boxing or general expression boxing
is introduced. I1b.2 adds the closed-local copy-then-use subset described below;
a typed destination alone does not establish eligibility.

### Scope of source evidence

Ordinary parameter and result declarations are source-contract boundaries.
Checking a loaded variable or a declared call result does not prove that every
previous assignment, helper argument or return body respected that declaration.
Source-call checking is therefore not complete descriptor/provenance checking.
Stores, returns and broader descriptor flow through casts, conditional merges
and aggregate members outside the directly checked actual expressions still
need separate gates. So do default/partial initialization, generic cache
correctness and unsupported carriers. Ordinary named aggregates containing
interface fields remain available but unqualified for this transfer guarantee,
including on special call routes. Their contents are not certified merely
because no direct descriptor formal is detected. Unsafe does not disable the checked call contract, but this
change does not redefine every other unsafe operation.

### Archive migration: ABI 8

Rebuild all Stasha archives and consumers for ABI 8. This is a semantic marker;
I0's slot ordering and the two-word descriptor layout are unchanged. A genuine
ABI7 archive can retain a mismatched source call inside its compiled body even
when newly checked declarations are imported successfully. ABI8 rejects source-backed Stasha
archives carrying incompatible metadata before importing/lowering/linking them.
This marker does not attest arbitrary foreign object files. Editing sidecar metadata
cannot make an old body satisfy the new contract; rebuild its source instead.

Runtime table membership, indirect-call CFI, object/table consistency, liveness,
ownership and concurrency remain independent requirements. This checkpoint does
not establish general memory safety or kernel readiness.


## I1b.2 checked local copies and projections

The bounded local-transfer check preserves the original concrete declaration
through admitted local producers, copies and assignments. A whole-body inventory
checks relevant writes before a particular interface use is admitted. Matching
storage sizes or similarly spelled generic instantiations do not establish the
same concrete type.

Supported copy-then-use paths include ordinary value fields, nested value fields,
and one fixed-array step. Each selected array element needs an explicit initial
witness, even if the eventual read uses a constant index. New local plans do not
cover generic or generated function bodies. For example, after a fully initialized local Holder
with an ordinary S value field, `stack let value = holder.value;` can supply the
existing identifier-valued interface boxing route when that exact use has a
closed local proof. Exact aliases and typed copies work under the same rule.
This does not add direct boxing syntax for arbitrary field/index expressions.

The proof is specific to the original expression, lexical binding and function
body. Address exposure of any descendant, array decay, deferred effects, or an
unknown mutation can prevent a new projection proof. A narrowly supported terminal
qualified receiver call may mutate the original receiver; it does not grant
eligibility to later identity uses. Primitive observations and independent copies
saved before the single direct, same-block, non-loop receiver frontier remain
available. Relevant unsupported multiassignment forms, including
repeated concrete targets, diagnose. Missing producers and incomplete inventories
cannot be treated as successful proofs. Existing access, bounds and permission
checks still apply.

Ordinary declared helper parameters/results and existing pointer-derived routes
remain explicit weaker source-contract boundaries. They do not seed the new
closed-local projection guarantee. A helper annotation, destination annotation,
or ordinary wrapper cannot establish the checked producer history of its fields.
Named aggregates containing interface descriptors remain usable but their nested
descriptor contents are unqualified. General helper/return/store closure, default
or partial descriptor initialization, ownership, lifetime, concurrency, table
membership and indirect-call CFI remain separate work.

### Archive migration: ABI 9

Rebuild all Stasha providers and consumers for ABI 9. This semantic compatibility
marker changes neither the two-word interface representation nor I0's slot order.
A genuine ABI8 archive can retain a mismatched concrete local store in its compiled
body even when its imported declarations look valid. Source-backed incompatible
archives must be rejected before their retained bodies are used. Relabeling
sidecar metadata is not a migration; rebuild the original source. The marker does
not attest arbitrary foreign object files.

### Separate known extent-query limitation

The I1b.2 compiler's fixed-array `len.(a)`/`cap.(a)` expression route could return zero rather than
the array extent. I1b.2 did not repair or qualify that expression behavior; its
local array proof uses the original declaration extent. This limitation is
separate from the checked index bounds and concrete transfer rules.

Reproduction retained for the follow-up review:

```sts
mod extent;
type I: interface { m(void): i32; };
fn main(void): i32 {
    stack i32 values[2] = .{17,24};
    ret len.(values) + cap.(values);
}
```

Baseline ABI8 and the initial I1b.2 Gate B compiler both emitted a constant-zero
return for the compact equivalent fixture (source SHA-256
`50d33538836e67dcbd2f51c27b6ef35c3b7158628767c0078e84f63323ab7d5e`).
That characterization was compile-only. Checked array indexing independently
uses the actual storage extent, so this query error does not remove its bounds
check. Explicit unchecked indexing remains outside that bound guarantee.

Follow-up review checklist:

- Repair fixed-array len/cap values without changing slice descriptor semantics.
- Cover aliases, fields and side-effecting array expressions; preserve single evaluation.
- Repeat checked-index boundary controls independently of query results.
- Retain the original failed test receipt separately from corrected I1b.2 tests.

The subsequent bounded query correction fixes supported newly compiled bodies;
see [fixed-array query semantics and rebuild guidance](Slices.md#fixed-array-and-slice-length-queries).
The original failure and baseline evidence remain historical. ABI9 archives are
still accepted and affected providers must be rebuilt. Within the closed-local
interface subset, compute the query-derived index in a typed `i32` local first;
inline len/cap index expressions remain unsupported source-proof forms.
