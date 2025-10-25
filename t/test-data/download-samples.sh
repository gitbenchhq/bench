#!/bin/bash
#
# Download Bench test data files
# This script fetches real bioinformatics files and generates synthetic test files
#
# Usage: ./download-samples.sh [--all|--real|--synthetic|--demo]
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "Bench Test Data Download Script"
echo "================================"
echo

# Parse arguments
DOWNLOAD_ALL=0
DOWNLOAD_REAL=0
DOWNLOAD_SYNTHETIC=0
DOWNLOAD_DEMO=0

if [ $# -eq 0 ]; then
    echo "Usage: $0 [--all|--real|--synthetic|--demo]"
    echo
    echo "Options:"
    echo "  --all        Download everything (real + synthetic)"
    echo "  --real       Download only real bioinformatics files"
    echo "  --synthetic  Generate only synthetic test files"
    echo "  --demo       Download/generate only files needed for Priority 0 demo"
    echo
    exit 1
fi

case "$1" in
    --all)
        DOWNLOAD_ALL=1
        DOWNLOAD_REAL=1
        DOWNLOAD_SYNTHETIC=1
        ;;
    --real)
        DOWNLOAD_REAL=1
        ;;
    --synthetic)
        DOWNLOAD_SYNTHETIC=1
        ;;
    --demo)
        DOWNLOAD_DEMO=1
        ;;
    *)
        echo "Error: Unknown option $1"
        exit 1
        ;;
esac

# Function to check if command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Function to download with progress
download_file() {
    local url="$1"
    local output="$2"

    if command_exists wget; then
        wget -O "$output" "$url"
    elif command_exists curl; then
        curl -L -o "$output" "$url"
    else
        echo "Error: Neither wget nor curl found. Please install one."
        exit 1
    fi
}

# Generate synthetic files
generate_synthetic() {
    echo "==> Generating synthetic test files..."

    if [ ! -f "test-100mb.bin" ]; then
        echo "    Creating test-100mb.bin (100MB)..."
        dd if=/dev/urandom of=test-100mb.bin bs=1M count=100 status=progress 2>&1 | tail -1
    else
        echo "    test-100mb.bin already exists, skipping"
    fi

    if [ ! -f "test-500mb.bin" ]; then
        echo "    Creating test-500mb.bin (500MB)..."
        dd if=/dev/urandom of=test-500mb.bin bs=1M count=500 status=progress 2>&1 | tail -1
    else
        echo "    test-500mb.bin already exists, skipping"
    fi

    if [ ! -f "test-1.5gb.bin" ]; then
        echo "    Creating test-1.5gb.bin (1.5GB) - for Priority 0 demo..."
        dd if=/dev/urandom of=test-1.5gb.bin bs=1M count=1536 status=progress 2>&1 | tail -1
    else
        echo "    test-1.5gb.bin already exists, skipping"
    fi

    if [ ! -f "test-2gb.bin" ]; then
        echo "    Creating test-2gb.bin (2GB) - for memory stress testing..."
        dd if=/dev/urandom of=test-2gb.bin bs=1M count=2048 status=progress 2>&1 | tail -1
    else
        echo "    test-2gb.bin already exists, skipping"
    fi

    echo "==> Synthetic files generated successfully"
    echo
}

# Download real bioinformatics files
download_real() {
    echo "==> Downloading real bioinformatics files..."

    # SARS-CoV-2 FASTQ from ENA (using smaller sample for testing)
    # Accession: ERR4145453 (Illumina, ~100MB compressed)
    if [ ! -f "sars-cov2.fastq" ] && [ ! -f "sars-cov2.fastq.gz" ]; then
        echo "    Downloading SARS-CoV-2 FASTQ sample (ERR4145453)..."
        echo "    This may take a few minutes (~100MB compressed)..."

        # Try ENA first
        if download_file "ftp://ftp.sra.ebi.ac.uk/vol1/fastq/ERR414/003/ERR4145453/ERR4145453.fastq.gz" "sars-cov2.fastq.gz"; then
            echo "    Decompressing..."
            gunzip sars-cov2.fastq.gz
            echo "    Download complete: sars-cov2.fastq"
        else
            echo "    Warning: Could not download from ENA"
            echo "    You can manually download using:"
            echo "      wget ftp://ftp.sra.ebi.ac.uk/vol1/fastq/ERR414/003/ERR4145453/ERR4145453.fastq.gz"
        fi
    else
        echo "    sars-cov2.fastq already exists, skipping"
    fi

    # Human chromosome reference (smaller chromosome for testing)
    # Chromosome 22 is ~50MB, good for testing
    if [ ! -f "human-chr22.fasta" ] && [ ! -f "human-chr22.fasta.gz" ]; then
        echo "    Downloading Human Chromosome 22 reference (~50MB)..."

        if download_file "ftp://ftp.ensembl.org/pub/release-109/fasta/homo_sapiens/dna/Homo_sapiens.GRCh38.dna.chromosome.22.fa.gz" "human-chr22.fasta.gz"; then
            echo "    Decompressing..."
            gunzip human-chr22.fasta.gz
            echo "    Download complete: human-chr22.fasta"
        else
            echo "    Warning: Could not download from Ensembl"
            echo "    You can manually download using:"
            echo "      wget ftp://ftp.ensembl.org/pub/release-109/fasta/homo_sapiens/dna/Homo_sapiens.GRCh38.dna.chromosome.22.fa.gz"
        fi
    else
        echo "    human-chr22.fasta already exists, skipping"
    fi

    echo "==> Real bioinformatics files downloaded successfully"
    echo
}

# Download demo-specific files
download_demo() {
    echo "==> Preparing Priority 0 demo files..."

    # For demo, we need 1.5GB file
    if [ ! -f "test-1.5gb.bin" ]; then
        echo "    Creating demo file: test-1.5gb.bin (1.5GB)..."
        dd if=/dev/urandom of=test-1.5gb.bin bs=1M count=1536 status=progress 2>&1 | tail -1
    else
        echo "    Demo file test-1.5gb.bin already exists, skipping"
    fi

    echo "==> Demo files ready"
    echo
}

# Execute based on options
if [ $DOWNLOAD_DEMO -eq 1 ]; then
    download_demo
elif [ $DOWNLOAD_ALL -eq 1 ]; then
    generate_synthetic
    download_real
else
    if [ $DOWNLOAD_SYNTHETIC -eq 1 ]; then
        generate_synthetic
    fi

    if [ $DOWNLOAD_REAL -eq 1 ]; then
        download_real
    fi
fi

# Summary
echo "==> Summary"
echo "Files in $SCRIPT_DIR:"
ls -lh *.bin *.fastq *.fasta 2>/dev/null || echo "  (no test files yet)"
echo
echo "Total disk usage:"
du -sh . 2>/dev/null || echo "  (directory size calculation failed)"
echo
echo "✓ Test data setup complete"
echo
echo "Note: These files are .gitignored and will not be committed to the repository"
echo "      See README.md for usage instructions"
