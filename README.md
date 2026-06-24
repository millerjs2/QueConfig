# QueConfig
QueConfig - pronounced "Que" + "Config"

## Dependencies

This project requires a C compiler (like `gcc`), `make`, and the `pkg-config` tool to build. It also relies on the following libraries:
- **GTK 4**
- **JSON-GLib**

### Installation on Ubuntu / Debian

You can install the required dependencies using `apt`:

```bash
sudo apt update
sudo apt install gcc make pkg-config libgtk-4-dev libjson-glib-dev
```

### Installation on Fedora

You can install the required dependencies using `dnf`:

```bash
sudo dnf install gcc make pkgconf gtk4-devel json-glib-devel
```

## Building

Once dependencies are installed, you can build the project by simply running:

```bash
make
```

This will produce the `QueConfig` executable.
