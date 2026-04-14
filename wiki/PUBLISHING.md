# How to Publish the Wiki to GitHub

Follow these steps exactly. The wiki will appear under the **Wiki** tab on your GitHub repository.

---

## Step 1 — Enable the Wiki on GitHub

1. Go to your repository on GitHub: `https://github.com/USERNAME/stasha`
2. Click **Settings** (the gear icon)
3. Scroll down to the **Features** section
4. Make sure **Wikis** is checked (enabled)

---

## Step 2 — Create the First Wiki Page (One-Time Setup)

GitHub won't let you clone the wiki repo until at least one page exists.

1. Go to your repo on GitHub
2. Click the **Wiki** tab
3. Click **Create the first page**
4. Write anything (e.g., "placeholder") and click **Save Page**

This creates the wiki git repository so you can clone it.

---

## Step 3 — Clone the Wiki Repository

The wiki is a **separate git repository** at:
```
https://github.com/USERNAME/stasha.wiki.git
```

```bash
cd ~/Documents        # or wherever you want to keep it
git clone https://github.com/USERNAME/stasha.wiki.git
cd stasha.wiki
```

Replace `USERNAME` with your actual GitHub username.

---

## Step 4 — Copy the Wiki Files

The wiki files are in the `wiki/` folder of the Stasha repo. Copy them into the cloned wiki repo:

```bash
cp -r /Users/jessicabruce/Documents/stasha/wiki/* ~/Documents/stasha.wiki/
```

---

## Step 5 — Review the Files

Check that all the right files are there:

```bash
ls ~/Documents/stasha.wiki/
```

You should see:
```
Home.md
Getting-Started.md
Language-Basics.md
Variables.md
Types.md
Functions.md
Control-Flow.md
Structs.md
Enums.md
Interfaces.md
Generics.md
Memory-Management.md
Safety-System.md
Error-Handling.md
Modules-and-Imports.md
Standard-Library.md
Concurrency.md
Project-System.md
Building-Projects.md
Debugging.md
Testing.md
Creating-Libraries.md
Using-Libraries.md
Linking-System.md
C-Interoperability.md
Compiler-Flags.md
How-the-Compiler-Works.md
Language-Reference.md
Roadmap.md
Contributing.md
_Sidebar.md
```

---

## Step 6 — Push to GitHub

```bash
cd ~/Documents/stasha.wiki
git add .
git commit -m "Add official Stasha language wiki"
git push
```

---

## Step 7 — View Your Wiki

Go to:
```
https://github.com/USERNAME/stasha/wiki
```

You'll see the **Home** page as the landing page. The sidebar (`_Sidebar.md`) will appear on every page for navigation.

---

## How It Works

- **Each `.md` file = one wiki page**
- The filename (without `.md`) becomes the page URL: `Home.md` → `https://github.com/USER/stasha/wiki/Home`
- **`Home.md`** is the wiki homepage (what you see when you click the Wiki tab)
- **`_Sidebar.md`** appears as the navigation sidebar on every page
- Links between pages use `[Text](Page-Name)` — no `.md` extension needed
- The wiki repo is at `https://github.com/USERNAME/stasha.wiki.git` (separate from the main repo)

---

## Updating the Wiki Later

Whenever you want to update a page:

```bash
cd ~/Documents/stasha.wiki
# edit the .md files
git add .
git commit -m "Update language reference"
git push
```

Changes appear on GitHub immediately after push.

---

## Keeping Wiki in Sync with the Repo

The wiki files live in `stasha/wiki/` in your main repo. A good workflow:

1. Edit files in `stasha/wiki/` (version-controlled with your code)
2. Copy changes to the wiki repo: `cp stasha/wiki/*.md stasha.wiki/`
3. Push from `stasha.wiki/`

Or set up a CI action to auto-sync on push to main.

---

## Troubleshooting

**"Repository not found" when cloning:**
→ Make sure you created the first page (Step 2)

**Push rejected:**
→ Make sure you have write access to the repository

**Sidebar not showing:**
→ The file must be named exactly `_Sidebar.md` (with underscore, capital S)

**Links between pages broken:**
→ Use `[Text](Page-Name)` format — the page name is the filename without `.md` and with spaces replaced by `-`
