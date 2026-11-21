"""Open the interactive round-screen simulator with synthetic Token Monitor data."""

import json
import time

from fixture_data import fixture
from watch_relay import snapshot


def main():
    from hermes_gadget.sim import Simulator
    from hermes_gadget.sim.window import SimulatorWindow

    sim = Simulator(board="sim-466x466-round")
    window = SimulatorWindow(sim, zoom=1)

    def update():
        now = int(time.time())
        data = fixture(now)
        compact = snapshot(data["/api/stats"], data["/api/history"], now)
        sim.console("tm inject " + json.dumps(compact, separators=(",", ":")))
        window.root.after(60_000, update)

    def initial():
        sim.console("tm mode watch")
        update()

    window.root.after(200, initial)
    window.run()


if __name__ == "__main__":
    main()
