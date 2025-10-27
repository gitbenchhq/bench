# Bench - Git for Large Files

**Version 1.0**

Bench is a Git-compatible version control system optimized for large files. It uses content-defined chunking to handle multi-GB files efficiently while maintaining full Git compatibility for regular workflows.

## Philosophy

Traditional version control systems struggle with large files because they store each version as a complete blob. Bench solves this by:

1. **Content-Defined Chunking**: Large files are automatically split into chunks using the Gear hash algorithm
2. **Intelligent Deduplication**: Only changed chunks are stored when files are modified
3. **Efficient Storage**: Multi-GB files with small changes use minimal additional storage
4. **Transparent Operation**: Works like Git - no special commands needed

## Key Differences from Git

- **Storage location**: `.bench/` directory instead of `.git/`
- **Large file handling**: Automatic chunking for files > 2MB
- **Performance**: Constant memory usage regardless of file size
- **Storage efficiency**: Only stores changed chunks, not entire files

## Quick Start

### Installation

**Linux/macOS:**
```bash
# Clone the repository
git clone https://github.com/gitbenchhq/bench.git
cd bench

# Build and install
make -j$(nproc)
sudo make install

# Verify installation
bench --version
```

### Basic Usage

Bench works exactly like Git for regular operations:

```bash
# Initialize a repository
bench init my-repo
cd my-repo

# Add and commit files (works for any size)
bench add large-file.bin
bench commit -m "Add large file"

# Standard Git operations work as expected
bench status
bench log
bench diff
```

## Demo: Efficient Large File Storage

Here's a demonstration of Bench's storage efficiency:

```bash
# Create a test repository
bench init demo
cd demo

# Add a 1GB file
dd if=/dev/urandom of=data.bin bs=1M count=1024
bench add data.bin
bench commit -m "Initial version"

# Check repository size
du -sh .bench/objects
# Output: ~1.0GB (compressed)

# Modify just 10MB in the middle of the file
dd if=/dev/urandom of=data.bin bs=1M count=10 seek=500 conv=notrunc
bench add data.bin
bench commit -m "Small modification"

# Check repository size again
du -sh .bench/objects
# Output: ~1.01GB (only the changed chunks stored!)

# Compare with Git: would be ~2GB for two full versions
```

**Result**: Only 10MB added for a 10MB change in a 1GB file, demonstrating 99% storage efficiency through chunk deduplication.

## Setting Up a Bench Server

You can easily set up a Bench server for backing up repositories or collaborating between multiple computers.

### Quick Server Setup (SSH-based)

The simplest approach uses SSH with a bare repository:

**On the server machine:**
```bash
# Create a directory for repositories
mkdir -p ~/bench-repos

# Create a bare repository
cd ~/bench-repos
bench init --bare myproject.bench

# The repository is now ready to receive pushes
```

**On the client machine:**
```bash
# Clone from the server
bench clone ssh://user@server/~/bench-repos/myproject.bench

# Or add as a remote to existing repository
bench remote add origin ssh://user@server/~/bench-repos/myproject.bench
bench push -u origin master
```

### Git Daemon Setup (Read-Only Access)

For read-only access without SSH authentication:

**On the server:**
```bash
# Export a repository for daemon access
cd ~/bench-repos/myproject.bench
touch git-daemon-export-ok

# Start the daemon (serves all repos in directory)
bench daemon --base-path=~/bench-repos --export-all --reuseaddr --verbose

# For background operation:
nohup bench daemon --base-path=~/bench-repos --export-all --reuseaddr > ~/bench-daemon.log 2>&1 &
```

**On the client:**
```bash
# Clone using git protocol
bench clone git://server/myproject.bench

# Note: This is read-only. Use SSH for push access.
```

### HTTP Server Setup (Apache/Nginx)

For HTTP(S) access with authentication:

**Apache configuration:**
```apache
# In /etc/apache2/sites-available/bench.conf
<VirtualHost *:80>
    ServerName bench.example.com
    DocumentRoot /var/bench-repos

    SetEnv GIT_PROJECT_ROOT /var/bench-repos
    SetEnv GIT_HTTP_EXPORT_ALL

    ScriptAlias /git/ /usr/libexec/bench-core/bench-http-backend/

    <Directory "/usr/libexec/bench-core/">
        Options +ExecCGI
        Require all granted
    </Directory>

    <LocationMatch "^/git/">
        AuthType Basic
        AuthName "Bench Repository"
        AuthUserFile /etc/apache2/bench.htpasswd
        Require valid-user
    </LocationMatch>
</VirtualHost>
```

```bash
# Create password file
sudo htpasswd -c /etc/apache2/bench.htpasswd username

# Enable site and restart
sudo a2ensite bench
sudo systemctl restart apache2
```

**On the client:**
```bash
bench clone https://username@bench.example.com/git/myproject.bench
```

### Simple Backup Setup

For personal backup to a second machine:

**1. Create backup repository on server:**
```bash
# On backup server
mkdir -p ~/backups/bench
cd ~/backups/bench
bench init --bare myproject-backup.bench
```

**2. Configure automatic backups on client:**
```bash
# On your working machine
cd ~/projects/myproject

# Add backup remote
bench remote add backup ssh://backup-server/~/backups/bench/myproject-backup.bench

# Push to backup
bench push backup master

# Optional: Create a backup script
cat > ~/bin/bench-backup.sh <<'EOF'
#!/bin/bash
cd ~/projects/myproject
bench push backup master --all --tags
echo "Backup completed at $(date)"
EOF
chmod +x ~/bin/bench-backup.sh

# Add to cron for automatic backups (daily at 2 AM)
crontab -e
# Add line: 0 2 * * * ~/bin/bench-backup.sh >> ~/bench-backup.log 2>&1
```

### Multi-Computer Sync

For working on the same project across multiple computers:

```bash
# On Server (acts as central hub)
bench init --bare ~/repos/project.bench

# On Computer 1
bench clone ssh://server/~/repos/project.bench
cd project
# ... work and commit ...
bench push origin master

# On Computer 2
bench clone ssh://server/~/repos/project.bench
cd project
# ... work and commit ...
bench push origin master

# Syncing changes
bench pull origin master  # Get latest changes from server
bench push origin master  # Send your changes to server
```

### Performance Tips for Large Files

When pushing/pulling large files over the network:

```bash
# Use compression (enabled by default)
bench config --global core.compression 9

# Increase network buffer (faster for large files)
bench config --global http.postBuffer 524288000  # 500MB

# For unstable connections, use resumable transfers
bench config --global http.lowSpeedLimit 1000
bench config --global http.lowSpeedTime 60
```

### Security Recommendations

- **Use SSH keys** instead of passwords for authentication
- **Restrict SSH access** with `~/.ssh/authorized_keys` restrictions
- **Use HTTPS** with valid certificates for public-facing servers
- **Enable firewall rules** to limit access to Git daemon port (9418)
- **Regular backups** of the server repositories

### Troubleshooting

**Connection refused:**
```bash
# Check if daemon is running
ps aux | grep bench-daemon

# Check firewall allows port 9418 (git protocol) or 22 (SSH)
sudo ufw allow 9418
sudo ufw allow 22
```

**Permission denied:**
```bash
# Ensure SSH keys are set up correctly
ssh-copy-id user@server

# Check repository permissions on server
ls -la ~/bench-repos/myproject.bench
```

**Large file transfers timing out:**
```bash
# Increase buffer and timeout settings
bench config --global http.postBuffer 1048576000  # 1GB
bench config --global http.timeout 600  # 10 minutes
```

## Documentation

- **[INSTALL.md](INSTALL.md)**: Detailed installation instructions
- **Git Documentation**: Most Git commands work identically in Bench

## Compatibility

### Git Compatibility Mode

Bench repositories are bit-perfect compatible with Git when using `--git-compat` mode:

```bash
# Create a Git-compatible repository (disables chunking)
bench init --git-compat my-repo

# This repository works identically to Git
# - No chunking for large files
# - Uses .bench/ directory (the only difference)
# - 100% compatible with standard Git tools
```

Use this mode when you need to:
- Collaborate with teams using standard Git
- Push to Git hosting services (GitHub, GitLab, etc.)
- Work with Git tools that don't understand Bench extensions

**Note**: Compatibility mode disables large file optimization. For large files, use standard Bench mode.

### Protocol & Platform Support

- **Protocol support**: SSH, HTTPS, file:// protocols
- **Platform support**: Linux, macOS, Windows (WSL)
- **Git interop**: Full compatibility with Git 2.0+

## License

Bench is an Open Source project covered by the GNU General Public License version 2. See [COPYING](COPYING) for details.

Bench is a fork of [Git](https://github.com/git/git), originally written by Linus Torvalds. Large file support and chunking implementation by Saman Amirpour Amraii at GitBench Inc.

## Contributing

See [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) and [SECURITY.md](SECURITY.md) for contribution guidelines and security policies.

---

**Need help?** Open an issue at https://github.com/gitbenchhq/bench/issues
