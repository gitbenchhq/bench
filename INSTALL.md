# Bench Installation Guide

## Quick Installation (Linux/macOS)

### Prerequisites

**Required:**
- GCC or Clang compiler
- GNU Make
- zlib development libraries

**Optional (for full features):**
- curl development libraries (for HTTPS support)
- OpenSSL development libraries (for secure protocols)
- Perl (for some helper scripts)

### Install Prerequisites

**Ubuntu/Debian:**
```bash
sudo apt-get update
sudo apt-get install build-essential libz-dev libcurl4-openssl-dev libssl-dev
```

**Fedora/RHEL/CentOS:**
```bash
sudo dnf install gcc make zlib-devel curl-devel openssl-devel
```

**macOS:**
```bash
# Install Xcode Command Line Tools
xcode-select --install

# Or use Homebrew
brew install gcc make
```

### Build and Install

1. **Clone the repository:**
   ```bash
   git clone https://github.com/gitbenchhq/bench.git
   cd bench
   ```

2. **Build Bench:**
   ```bash
   # Use all available CPU cores for faster build
   make -j$(nproc)
   ```

   This typically takes 1-2 minutes on modern hardware.

3. **Install Bench:**

   **System-wide installation (requires sudo):**
   ```bash
   sudo make install
   ```
   This installs to `/usr/local/bin/` by default.

   **User-local installation (no sudo needed):**
   ```bash
   make prefix=$HOME install
   ```
   This installs to `~/bin/`. Make sure `~/bin` is in your `$PATH`.

   **Custom prefix:**
   ```bash
   make prefix=/opt/bench install
   ```

4. **Verify installation:**
   ```bash
   bench --version
   # Should output: bench version 1.0
   ```

### Build Options

**Minimal build (no optional dependencies):**
```bash
make NO_CURL=1 NO_OPENSSL=1
```

**Debug build (for development):**
```bash
make CFLAGS="-g -O0"
```

**Optimized build with profile feedback (slower build, faster runtime):**
```bash
make profile-install
```

## Testing Your Installation

Run the test suite to verify everything works:

```bash
# Quick smoke test
cd t
./t0000-basic.sh

# Test chunking functionality
./helper/test-tool bench-core
```

## Updating Bench

To update to a newer version:

```bash
cd bench
git pull origin master
make clean
make -j$(nproc)
sudo make install
```

## Uninstalling

To remove Bench from your system:

```bash
cd bench
sudo make uninstall
```

Or manually remove the installed binaries:
```bash
sudo rm -rf /usr/local/bin/bench* /usr/local/libexec/bench-core
```

## Platform-Specific Notes

### Linux

Bench works on all major Linux distributions. On older systems, you may need to update GCC to version 4.9 or later.

### macOS

- Xcode Command Line Tools are required
- On M1/M2 Macs, use `nproc` or `sysctl -n hw.ncpu` to get core count
- Installation paths may differ: check with `which bench`

### Windows

Bench is not natively supported on Windows. Use one of these options:
- **WSL2** (Windows Subsystem for Linux) - Recommended
- **Git Bash/MSYS2** - May work but not officially supported
- **Docker** - Run Bench in a Linux container

**WSL2 installation:**
```bash
# Inside WSL2 Ubuntu
sudo apt-get update
sudo apt-get install build-essential libz-devel libcurl4-openssl-dev
git clone https://github.com/gitbenchhq/bench.git
cd bench
make -j$(nproc)
sudo make install
```

## Troubleshooting

### Build Errors

**Error: `zlib.h: No such file or directory`**
- Solution: Install zlib development package (see Prerequisites)

**Error: `curl/curl.h: No such file or directory`**
- Solution: Either install libcurl-dev or build with `make NO_CURL=1`

**Error: Permission denied during `make install`**
- Solution: Use `sudo make install` or install to `$HOME` with `make prefix=$HOME install`

### Runtime Issues

**Command not found: `bench`**
- Solution: Ensure installation directory is in your `$PATH`
- Check with: `echo $PATH`
- Add to `~/.bashrc` or `~/.zshrc`: `export PATH="$HOME/bin:$PATH"`

**Bench is slow or uses too much memory**
- Check version: `bench --version` (should be 1.0 or later)
- Large file chunking was significantly optimized in version 1.0

## Advanced Configuration

### Custom Makefile Variables

You can create a `config.mak` file in the bench directory to customize your build:

```makefile
# Example config.mak
prefix = /opt/bench
NO_CURL = 1
NO_OPENSSL = 1
CFLAGS = -O3 -march=native
```

Then run:
```bash
make
make install
```

### Configuration Options

After installation, configure Bench like Git:

```bash
# Set your identity
bench config --global user.name "Your Name"
bench config --global user.email "you@example.com"

# Set default editor
bench config --global core.editor vim

# Configure chunking (optional)
bench config --global chunk.minSize 2097152  # 2MB default
bench config --global chunk.targetSize 16777216  # 16MB default
```

## Getting Help

- **Documentation**: See [README.md](README.md) for usage examples
- **Issues**: Report bugs at https://github.com/gitbenchhq/bench/issues
- **Git Documentation**: Most Git documentation applies to Bench

## Next Steps

Once installed, see [README.md](README.md) for a quick start guide and examples of using Bench with large files.
