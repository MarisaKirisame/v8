import random
import math
import json
import subprocess
import time
import numpy
import matplotlib.pyplot as plt

#min_heap_size = 2359296
min_heap_size = 5e7
#max_heap_size = 4e9
max_heap_size = 1e9
def exponential_sample_in_range(lower, upper):
    return int(math.exp(random.uniform(math.log(lower), math.log(upper))))

def run_experiment():
    before = time.time()
    subprocess.run(["./hello_world"], check=True)
    after = time.time()
    return after - before

heap_sizes = []
times = []

for code_path in ["splay.js", "pdfjs.js"]:
    for i in range(1000):
        json_map = {}
        heap_size = exponential_sample_in_range(min_heap_size, max_heap_size)
        json_map["heap_size"] = heap_size
        json_map["code_path"] = code_path
        with open("balancer-config", "w") as f:
            json.dump(json_map, f)
        try:
            time_taken = run_experiment()
            heap_sizes.append(heap_size)
            times.append(time_taken)
        except subprocess.CalledProcessError:
            print(f"heap_size too small! {heap_size}")
            raise

plt.scatter(heap_sizes, times)
plt.savefig('plot.png')
