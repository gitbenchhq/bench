# Bench Test Data

This directory contains large test files for bench validation and customer demos.

## Purpose

These files are used for:
- **Priority 0**: Customer demo showing Git breaking vs Bench working
- **Priority 1**: Core functionality tests (chunking correctness, version handling)
- **Priority 2**: Performance validation (memory usage, pack efficiency)
- **Priority 3**: Edge case testing (compressed formats, boundary stability)

## Why Files Are Not Tracked

Large test files (100MB - 2GB) are excluded from Git tracking via `.gitignore` because:
1. Git repositories cannot efficiently handle large binary files
2. This directory would bloat the repository size
3. GitBench will eventually host these files once we have our own infrastructure

**Future Work**: Once GitBench hosting is available, this directory will be tracked by Bench itself, and the `.gitignore` will be removed. See CLAUDE.md "Future Enhancements" section.

## Test Files

### Real Bioinformatics Files (Recommended)

These files provide realistic testing scenarios for customer demos:

1. **SARS-CoV-2 Genome FASTQ** (~200MB)
   - Source: European Nucleotide Archive (ENA)
   - Download: Use `fastq-dl` or direct wget from ENA FTP
   - Example accession: SRR11140748 (Ion Torrent S5, whole genome sequencing)
   - License: Public domain / CC0

2. **Human Reference Genome (Chromosome 1)** (~250MB uncompressed)
   - Source: 1000 Genomes Project or NCBI RefSeq
   - Format: FASTA
   - Download: `wget ftp://ftp.1000genomes.ebi.ac.uk/vol1/ftp/technical/reference/GRCh38_reference_genome/`
   - License: Public domain

3. **RNA-Seq FASTQ** (~500MB-1GB)
   - Source: NCBI SRA or ENA
   - Use SRA Explorer to find suitable datasets
   - Example: RNA-seq from model organisms (yeast, C. elegans)
   - License: Varies by dataset (check SRA metadata)

### Download Scripts

```bash
# Install fastq-dl (Python tool for downloading from SRA/ENA)
pip install fastq-dl

# Download SARS-CoV-2 sample
fastq-dl --accession SRR11140748 --outdir t/test-data/

# Alternative: Direct wget from ENA
wget -P t/test-data/ ftp://ftp.sra.ebi.ac.uk/vol1/fastq/SRR111/048/SRR11140748/SRR11140748.fastq.gz

# Decompress if needed
gunzip t/test-data/SRR11140748.fastq.gz
```

### Synthetic Files (Fallback)

If real data is unavailable, generate synthetic test files:

```bash
cd t/test-data/

# 100MB random file
dd if=/dev/urandom of=test-100mb.bin bs=1M count=100

# 500MB random file
dd if=/dev/urandom of=test-500mb.bin bs=1M count=500

# 1.5GB file (for Priority 0 demo)
dd if=/dev/urandom of=test-1.5gb.bin bs=1M count=1536

# 2GB file (for memory stress testing)
dd if=/dev/urandom of=test-2gb.bin bs=1M count=2048
```

## File Organization

```
t/test-data/
├── README.md                  # This file
├── .gitignore                 # Excludes large files from Git
├── *.fastq                    # Real bioinformatics FASTQ files
├── *.fasta                    # Real genome sequence files
├── *.bin                      # Synthetic test files
└── download-samples.sh        # Script to fetch all test files
```

## Usage in Tests

Test helpers in `t/helper/` reference these files:

```c
// Example from test-phase7-demo.c
const char *test_file = "../../test-data/test-1.5gb.bin";
```

Shell test scripts reference them:

```bash
# From t/t9100-bench-demo.sh
TEST_FILE="$TEST_DIRECTORY/test-data/sars-cov2.fastq"
```

## License Information

- **Synthetic files** (generated with dd): No copyright, can be freely used
- **Public bioinformatics data**: Most SRA/ENA data is public domain or CC0
- **Always verify**: Check specific dataset licenses before using in commercial demos

## Future: GitBench Hosting

Once Bench has its own hosting infrastructure:

1. Upload these files to GitBench repository
2. Remove `.gitignore` exclusions
3. Track files using Bench's content-defined chunking
4. Demonstrate Bench managing its own large test files