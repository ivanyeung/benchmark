# Running the benchmark in Docker (macOS host)

Docker Desktop for Mac runs containers inside a real Linux VM (LinuxKit), so
a container here is genuine Linux with cgroup v2 — no approximation needed,
unlike a native macOS port. This doc is the tested, working procedure.

## 1. Prereqs

Docker Desktop installed and running (`docker info` should succeed).

## 2. Dockerfile

Already added at the repo root:

```dockerfile
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    build-essential \
    fio \
    sysstat \
    python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
```

No `COPY` — the repo is bind-mounted at runtime so edits on the Mac are
visible in the container immediately without rebuilding the image.

## 3. Build the image

```bash
docker build -t pcf-bench .
```

## 4. Run the container with cgroup access

```bash
docker run -it --rm \
  --privileged \
  --cgroupns=private \
  -v "$(pwd)":/app \
  -w /app \
  pcf-bench bash
```

- `--privileged` gives the container write access to its delegated cgroup
  subtree (needed for `mkdir_p_cgroup` / `write_cgroup_file` in
  `benchmark.c`).
- `--cgroupns=private` gives the container its own cgroup v2 namespace
  rooted at `/sys/fs/cgroup`.

## 5. Required extra step — free the root cgroup before enabling controllers

**This step is not optional.** Verified by testing: on first run, every
`memory.low`/`io.weight` write failed with `Permission denied`, and
`cgroup.subtree_control` write failed with `Device or resource busy`.

Cause: the container's own shell (and PID 1) live *directly* in the
container's root cgroup (`/sys/fs/cgroup/cgroup.procs`). cgroup v2 enforces
a "no internal processes" rule — a cgroup can't both hold processes itself
*and* delegate controllers to children. Since our shell sits at the root,
the root can't enable `+memory +io` for its children until the shell is
moved out.

Fix — move the current shell into its own leaf cgroup first, then enable
controllers:

```bash
mkdir -p /sys/fs/cgroup/init
echo $$ > /sys/fs/cgroup/init/cgroup.procs
echo "+memory +io" > /sys/fs/cgroup/cgroup.subtree_control
```

Verified output after this:
```
$ cat /sys/fs/cgroup/cgroup.subtree_control
io memory
```

Do this once per container session, before running `./benchmark`.

## 6. Build and run the benchmark

```bash
make
./benchmark -v --cgroup-config cgroup_isolated.ini -m direct dual
python3 benchmark_analysis.py benchmark_results/
```

## 7. Verify clients actually landed in the right cgroup

While a run is in progress, in a second shell into the same container
(`docker exec -it <container> bash`):

```bash
cat /sys/fs/cgroup/client1_steady/cgroup.procs
```

Verified real output during a test run — actual fio PIDs, not empty:
```
12
47
48
51
53
```

`memory.low` was also confirmed applied:
```bash
$ cat /sys/fs/cgroup/client1_steady/memory.low
1073741824    # = 1G, matches cgroup_isolated.ini
```

## 8. Known limitation — `io.weight` is not available

Even after step 5 succeeds (`io` shows in `cgroup.subtree_control`), the
per-cgroup `io.weight` file does not exist:

```
$ cat /sys/fs/cgroup/client1_steady/io.weight
cat: /sys/fs/cgroup/client1_steady/io.weight: No such file or directory
```

The `io` controller only exposes weight files for block devices whose
request-queue scheduler supports cgroup weighting (e.g. `bfq`). Docker
Desktop's virtualized disk (overlay2 over virtiofs) doesn't run such a
scheduler, so `io.weight` never appears no matter how the container is
started — this is a host-VM property, not a container permissions issue.

Practical effect: **`memory.max`/`memory.low` fairness levers work
correctly in Docker Desktop; `io.weight`-based experiments do not.** For
Mechanism-2-style io-weight experiments, run on a real Linux host/VM
(bare metal, or a VM like Multipass/Lima/UTM with a real virtio-blk or
physical device using `bfq`) instead of Docker Desktop.

## 9. General caveat

Docker Desktop's storage path (overlay2 over virtiofs) and the VM's
virtualized disk add I/O latency/variance a bare-metal Linux box wouldn't
have. Fine for validating the harness and cgroup logic; for numbers you'd
actually trust for the fairness measurements themselves, prefer a real
Linux host or VM.
