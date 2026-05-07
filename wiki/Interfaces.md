# Interfaces

Interfaces define a contract — a set of methods that a type must implement. Stasha uses interfaces for polymorphism without virtual dispatch overhead (implementations are statically resolved where possible).

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
    name(void): stack i8 *r;
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

If the struct doesn't implement all required methods, the compiler errors.

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
