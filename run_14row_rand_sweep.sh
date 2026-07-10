#!/bin/bash
set -e
cd /app

run_row() {
  local desc="$1"; local cgroup_cfg="$2"; local outdir="$3"; local target="$4"
  echo "=== $desc -> $outdir ==="
  rm -rf "$outdir"
  ./benchmark -v --cgroup-config "$cgroup_cfg" -m cached -o "$outdir" $target
}

run_row "row1 client1 alone (isolated)"                       cgroup_isolated.ini results/row1_rand_client1     client1_steady
run_row "row2 client2 alone (isolated, randread)"             cgroup_isolated.ini results/row2_rand_client2     client2_noisy
run_row "row3 client2 alone (isolated, randread - was seq)"   cgroup_isolated.ini results/row3_rand_client2     client2_noisy
run_row "row4 client2 alone (isolated, randread - was randwrite)" cgroup_isolated.ini results/row4_rand_client2 client2_noisy

run_row "row5 dual isolated (randread - was seq)"             cgroup_isolated.ini results/row5_rand_isolated    dual
run_row "row6 dual isolated (randread)"                       cgroup_isolated.ini results/row6_rand_isolated    dual
run_row "row7 dual isolated (randread - was randwrite)"       cgroup_isolated.ini results/row7_rand_isolated    dual

run_row "row8 dual shared-nocap (randread - was seq)"         cgroup_shared.ini   results/row8_rand_shared      dual
run_row "row9 dual shared-nocap (randread)"                   cgroup_shared.ini   results/row9_rand_shared      dual
run_row "row10 dual shared-nocap (randread - was randwrite)"  cgroup_shared.ini   results/row10_rand_shared     dual

echo "--- adding client1 memory.max=1G to cgroup_shared.ini for rows 11-12 ---"
python3 - <<'PYEOF'
p = "cgroup_shared.ini"
s = open(p).read()
s = s.replace("[client1_steady]\ncgroup_name = clients/client1_steady\n",
              "[client1_steady]\ncgroup_name = clients/client1_steady\nmemory.max = 1G\n")
open(p, "w").write(s)
PYEOF
cat cgroup_shared.ini

run_row "row11 dual shared c1cap=1G (randread)"                cgroup_shared.ini results/row11_rand_c1cap      dual
run_row "row12 dual shared c1cap=1G (randread - was randwrite)" cgroup_shared.ini results/row12_rand_c1cap     dual

echo "--- reverting client1 cap, adding client2 memory.max=1G for rows 13-14 ---"
python3 - <<'PYEOF'
p = "cgroup_shared.ini"
s = open(p).read()
s = s.replace("[client1_steady]\ncgroup_name = clients/client1_steady\nmemory.max = 1G\n",
              "[client1_steady]\ncgroup_name = clients/client1_steady\n")
s = s.replace("[client2_noisy]\ncgroup_name = clients/client2_noisy\n",
              "[client2_noisy]\ncgroup_name = clients/client2_noisy\nmemory.max = 1G\n")
open(p, "w").write(s)
PYEOF
cat cgroup_shared.ini

run_row "row13 dual shared c2limited=1G (randread)"                cgroup_shared.ini results/row13_rand_c2limited dual
run_row "row14 dual shared c2limited=1G (randread - was randwrite)" cgroup_shared.ini results/row14_rand_c2limited dual

echo "--- reverting cgroup_shared.ini to clean (no-cap) state ---"
python3 - <<'PYEOF'
p = "cgroup_shared.ini"
s = open(p).read()
s = s.replace("[client2_noisy]\ncgroup_name = clients/client2_noisy\nmemory.max = 1G\n",
              "[client2_noisy]\ncgroup_name = clients/client2_noisy\n")
open(p, "w").write(s)
PYEOF
cat cgroup_shared.ini

echo "ALL 14 ROWS COMPLETE"