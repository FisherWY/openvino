#
# silu paddle model generator
#
import numpy as np
from save_model import saveModel
import paddle as pdpd
import sys


def silu(name: str, x, data_type):
    pdpd.enable_static()

    with pdpd.static.program_guard(pdpd.static.Program(), pdpd.static.Program()):
        node_x = pdpd.static.data(
            name='input_x', shape=x.shape, dtype=data_type)
        out = pdpd.fluid.layers.silu(x=node_x, name='silu')

        cpu = pdpd.static.cpu_places(1)
        exe = pdpd.static.Executor(cpu[0])
        # startup program will call initializer to initialize the parameters.
        exe.run(pdpd.static.default_startup_program())

        outs = exe.run(
            feed={'input_x': x},
            fetch_list=[out])

        saveModel(name, exe, feedkeys=['input_x'], fetchlist=[out],
                  inputs=[x], outputs=[outs[0]], target_dir=sys.argv[1])

    return outs[0]


def main():
    data_type = 'float32'
    x = np.random.randn(2, 3).astype(data_type)
    silu("silu", x, data_type)


if __name__ == "__main__":
    main()
