package com.example.arthooks;

import android.util.Log;

import com.arthooks.ArtHooks;

import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;

/**
 * Measures steady-state throughput, so the cost of giving up AOT code can be quantified rather than
 * guessed at.
 *
 * <p>Run with {@code tools/run-benchmark.sh}, which drives it across compiler filters. It does not
 * run on a normal launch: {@link MainActivity} only starts it for an explicit {@code --ez benchmark
 * true}, and then skips the demo hook and the self-test so that nothing else is competing.
 *
 * <p>Each workload is run {@link #ROUNDS} times. The first round is reported separately because it
 * is the one that runs before the JIT has caught up — under a {@code verify} build or after
 * {@link ArtHooks#disable_aot()} that is nterp, and it is the number people actually feel at
 * startup. The median of the remaining rounds is steady state, where the JIT has had its turn.
 *
 * <p>The workloads are split deliberately. {@code accessors} and {@code fib} are call-heavy and are
 * the ones inlining decides, which matters because a Java-debuggable runtime compiles without
 * inlining at all. {@code sort_library}, {@code strings} and {@code hashmap} spend most of their
 * time in the boot classpath, which is the part {@code DeoptimizeBootImage()} gives up. The rest is
 * ordinary app-side loop and array work.
 */
final class Benchmark {
    private static final String TAG = "HookBenchmark";

    private static final int ROUNDS = 11;

    /** Every workload folds into this, so nothing can be eliminated as dead. */
    static long sink;

    private Benchmark() {
    }

    interface Workload {
        long run();
    }

    static void run() {
        new Thread(Benchmark::measure, "arthooks-benchmark").start();
    }

    private static void measure() {
        Log.i(TAG, "BEGIN aot_disabled=" + ArtHooks.is_aot_disabled()
                + " available=" + ArtHooks.is_available());

        time("accessors", Benchmark::accessors);
        time("fib", Benchmark::fib);
        time("sieve", Benchmark::sieve);
        time("quicksort", Benchmark::quicksort);
        time("sort_library", Benchmark::sort_library);
        time("matmul", Benchmark::matmul);
        time("bitops", Benchmark::bitops);
        time("strings", Benchmark::strings);
        time("hashmap", Benchmark::hashmap);

        Log.i(TAG, "END sink=" + sink);
    }

    /** Runs one workload ROUNDS times and reports the first round and the median of the rest. */
    private static void time(String name, Workload workload) {
        long[] micros = new long[ROUNDS];
        for (int round = 0; round < ROUNDS; round++) {
            long start = System.nanoTime();
            sink += workload.run();
            micros[round] = (System.nanoTime() - start) / 1000L;

            // The JIT compiles on its own thread; give it a moment to install what this round made
            // hot, or every round measures the same not-yet-compiled code.
            try {
                Thread.sleep(25);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return;
            }
        }

        long[] rest = Arrays.copyOfRange(micros, 1, ROUNDS);
        Arrays.sort(rest);
        Log.i(TAG, String.format("RESULT %-13s first=%8dus  steady=%8dus  best=%8dus",
                name, micros[0], rest[rest.length / 2], rest[0]));
    }

    // --- call-heavy: what inlining decides ---------------------------------------------------

    static final class Point {
        private int x;
        private int y;

        Point(int x, int y) {
            this.x = x;
            this.y = y;
        }

        int get_x() {
            return x;
        }

        int get_y() {
            return y;
        }

        void set_x(int value) {
            x = value;
        }
    }

    private static long accessors() {
        Point point = new Point(1, 2);
        long total = 0;
        for (int i = 0; i < 2_000_000; i++) {
            point.set_x(i);
            total += point.get_x() + point.get_y();
        }
        return total;
    }

    private static int fibonacci(int n) {
        return (n < 2) ? n : fibonacci(n - 1) + fibonacci(n - 2);
    }

    private static long fib() {
        return fibonacci(26);
    }

    // --- app-side loops and arrays ---------------------------------------------------------------

    private static long sieve() {
        final int limit = 2_000_000;
        boolean[] composite = new boolean[limit];
        long primes = 0;
        for (int i = 2; i < limit; i++) {
            if (!composite[i]) {
                primes++;
                for (long j = (long) i * i; j < limit; j += i) {
                    composite[(int) j] = true;
                }
            }
        }
        return primes;
    }

    private static int[] scrambled(int count) {
        int[] values = new int[count];
        int state = 0x12345678;
        for (int i = 0; i < count; i++) {
            state ^= state << 13;
            state ^= state >>> 17;
            state ^= state << 5;
            values[i] = state;
        }
        return values;
    }

    private static void sort_range(int[] values, int low, int high) {
        if (low >= high) {
            return;
        }
        int pivot = values[(low + high) >>> 1];
        int i = low;
        int j = high;
        while (i <= j) {
            while (values[i] < pivot) {
                i++;
            }
            while (values[j] > pivot) {
                j--;
            }
            if (i <= j) {
                int swap = values[i];
                values[i] = values[j];
                values[j] = swap;
                i++;
                j--;
            }
        }
        sort_range(values, low, j);
        sort_range(values, i, high);
    }

    private static long quicksort() {
        int[] values = scrambled(300_000);
        sort_range(values, 0, values.length - 1);
        return values[0] + values[values.length - 1];
    }

    private static long sort_library() {
        int[] values = scrambled(300_000);
        Arrays.sort(values);
        return values[0] + values[values.length - 1];
    }

    private static long matmul() {
        final int n = 120;
        double[][] a = new double[n][n];
        double[][] b = new double[n][n];
        double[][] c = new double[n][n];
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n; j++) {
                a[i][j] = (i * 31 + j) % 17;
                b[i][j] = (i * 13 + j) % 23;
            }
        }
        for (int i = 0; i < n; i++) {
            for (int k = 0; k < n; k++) {
                double aik = a[i][k];
                for (int j = 0; j < n; j++) {
                    c[i][j] += aik * b[k][j];
                }
            }
        }
        return (long) c[n - 1][n - 1];
    }

    private static long bitops() {
        int hash = 0x811C9DC5;
        for (int i = 0; i < 5_000_000; i++) {
            hash ^= i;
            hash *= 0x01000193;
            hash = Integer.rotateLeft(hash, 7);
        }
        return hash;
    }

    // --- boot-classpath heavy --------------------------------------------------------------------

    private static long strings() {
        StringBuilder builder = new StringBuilder();
        for (int i = 0; i < 40_000; i++) {
            builder.append(i).append(',');
        }
        String joined = builder.toString();
        long total = joined.length();
        total += joined.indexOf("39999");
        total += joined.substring(0, 1000).hashCode();
        for (String part : joined.split(",", 2000)) {
            total += part.length();
        }
        return total;
    }

    private static long hashmap() {
        Map<Integer, Integer> map = new HashMap<>();
        for (int i = 0; i < 200_000; i++) {
            map.put(i, i * 3);
        }
        long total = 0;
        for (int i = 0; i < 200_000; i++) {
            Integer value = map.get(i);
            if (value != null) {
                total += value;
            }
        }
        return total;
    }
}
