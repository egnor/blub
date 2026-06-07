import marimo

__generated_with = "0.3.2"
app = marimo.App()


@app.cell
def __(beam_mass, beam_stress, beams, materials, mo, units):
    rows = {}
    mats = {
        "Fe": materials["ASTM A53 Grade A"],
        "Al": materials["Aluminum 6063-T5"],
    }
    for beam in beams.values():
        row = rows.setdefault(beam.nominal, {"Nominal": f"{beam.nominal} nom"})
        stress_10ft = beam_stress(10 * units.foot, beam)
        for mat_name, material in mats.items():
            yield_10ft = (material.strength / stress_10ft).to("lbf")
            mass_10ft = beam_mass(beam, 10 * units.foot, material).to("lb")
            sys_name = beam.system.replace("ch ", "")
            yield_text = f"{yield_10ft:.0f}".replace(" ", "")
            mass_text = f"{mass_10ft:.0f}".replace(" ", "")        
            text = f"**{yield_text}**  \n({mass_text})"
            row[f"{sys_name}:{mat_name}"] = mo.md(text)

    mo.ui.table(
        data=list(rows.values()),
        pagination=False,
        selection=None,
        label="## 10ft tube bending yield force (and tube weight)",
    )
    return (
        beam,
        mass_10ft,
        mass_text,
        mat_name,
        material,
        mats,
        row,
        rows,
        stress_10ft,
        sys_name,
        text,
        yield_10ft,
        yield_text,
    )


@app.cell(hide_code=True)
def __(math, mo):
    def far_end_moment(load, length):
        return load * length

    def beam_inertia(beam):
        if beam.outer_d and beam.wall_t:
            inner_d = beam.outer_d - 2 * beam.wall_t
            return math.pi / 64 * (beam.outer_d ** 4 - inner_d ** 4)
        else:
            raise ValueError(f"No diameter/thickness: {beam}")

    def beam_fiber_radius(beam):
        if beam.outer_d:
            return beam.outer_d / 2
        else:
            raise ValueError(f"No diameter: {beam}")

    def beam_stress(bending_moment, beam):
        return bending_moment * beam_fiber_radius(beam) / beam_inertia(beam)

    def beam_area(beam):
        if beam.outer_d:
            return math.pi * beam.outer_d * beam.wall_t
        else:
            raise ValueError(f"No diameter: {beam}")

    def beam_volume(beam, length):
        return beam_area(beam) * length

    def beam_mass(beam, length, material):
        return beam_volume(beam, length) * material.density

    _funs = {n for n, v in globals().items() if isinstance(v, type(beam_stress))}
    mo.md(f"🧮 Functions: {', '.join(_funs)}")
    return (
        beam_area,
        beam_fiber_radius,
        beam_inertia,
        beam_mass,
        beam_stress,
        beam_volume,
        far_end_moment,
    )


@app.cell
def __(Optional, mo, pint, pydantic_dataclass, units):
    @pydantic_dataclass
    class Beam:
        system: str
        nominal: str
        outer_d: Optional[pint.Quantity] = None
        wall_t: Optional[pint.Quantity] = None

    _in = units.inch
    beams = {(beam.system, beam.nominal): beam for beam in [
        Beam("Sch 40", "1/8", 0.405 * _in, 0.068 * _in),
        Beam("Sch 40", "1/4", 0.540 * _in, 0.088 * _in),
        Beam("Sch 40", "3/8", 0.675 * _in, 0.091 * _in),
        Beam("Sch 40", "1/2", 0.840 * _in, 0.109 * _in),
        Beam("Sch 40", "3/4", 1.050 * _in, 0.113 * _in),
        Beam("Sch 40", "1", 1.315 * _in, 0.133 * _in),
        Beam("Sch 40", "1 1/4", 1.660 * _in, 0.140 * _in),
        Beam("Sch 40", "1 1/2", 1.900 * _in, 0.145 * _in),
        Beam("Sch 40", "2", 2.375 * _in, 0.154 * _in),
        Beam("Sch 40", "2 1/2", 2.875 * _in, 0.203 * _in),
        Beam("Sch 40", "3", 3.500 * _in, 0.216 * _in),
        Beam("Sch 40", "3 1/2", 4.000 * _in, 0.226 * _in),
        Beam("Sch 40", "4", 4.500 * _in, 0.237 * _in),
        Beam("Sch 80", "1/8", 0.405 * _in, 0.095 * _in),
        Beam("Sch 80", "1/4", 0.540 * _in, 0.119 * _in),
        Beam("Sch 80", "3/8", 0.675 * _in, 0.126 * _in),
        Beam("Sch 80", "1/2", 0.840 * _in, 0.147 * _in),
        Beam("Sch 80", "3/4", 1.050 * _in, 0.154 * _in),
        Beam("Sch 80", "1", 1.315 * _in, 0.179 * _in),
        Beam("Sch 80", "1 1/4", 1.660 * _in, 0.191 * _in),
        Beam("Sch 80", "1 1/2", 1.900 * _in, 0.200 * _in),
        Beam("Sch 80", "2", 2.375 * _in, 0.218 * _in),
        Beam("Sch 80", "2 1/2", 2.875 * _in, 0.276 * _in),
        Beam("Sch 80", "3", 3.500 * _in, 0.300 * _in),
        Beam("Sch 80", "3 1/2", 4.000 * _in, 0.318 * _in),
        Beam("Sch 80", "4", 4.500 * _in, 0.337 * _in),
        Beam("EMT", "1/2", 0.706 * _in, 0.042 * _in),
        Beam("EMT", "3/4", 0.922 * _in, 0.049 * _in),
        Beam("EMT", "1", 1.163 * _in, 0.057 * _in),
        Beam("EMT", "1 1/4", 1.510 * _in, 0.065 * _in),
        Beam("EMT", "1 1/2", 1.740 * _in, 0.065 * _in),
        Beam("EMT", "2", 2.197 * _in, 0.065 * _in),
        Beam("EMT", "2 1/2", 2.875 * _in, 0.072 * _in),
        Beam("EMT", "3", 3.500 * _in, 0.072 * _in),
        Beam("EMT", "3 1/2", 4.000 * _in, 0.083 * _in),
        Beam("EMT", "4", 4.500 * _in, 0.083 * _in),
        Beam("IMC", "1/2", 0.815 * _in, 0.078 * _in),
        Beam("IMC", "3/4", 1.029 * _in, 0.083 * _in),
        Beam("IMC", "1", 1.290 * _in, 0.093 * _in),
        Beam("IMC", "1 1/4", 1.638 * _in, 0.095 * _in),
        Beam("IMC", "1 1/2", 1.883 * _in, 0.100 * _in),
        Beam("IMC", "2", 2.360 * _in, 0.105 * _in),
        Beam("IMC", "2 1/2", 2.857 * _in, 0.150 * _in),
        Beam("IMC", "3", 3.476 * _in, 0.150 * _in),
        Beam("IMC", "3 1/2", 3.971 * _in, 0.150 * _in),
        Beam("IMC", "4", 4.466 * _in, 0.150 * _in),
        Beam("RMC", "1/2", 0.840 * _in, 0.104 * _in),
        Beam("RMC", "3/4", 1.050 * _in, 0.107 * _in),
        Beam("RMC", "1", 1.315 * _in, 0.126 * _in),
        Beam("RMC", "1 1/4", 1.660 * _in, 0.133 * _in),
        Beam("RMC", "1 1/2", 1.900 * _in, 0.138 * _in),
        Beam("RMC", "2", 2.375 * _in, 0.146 * _in),
        Beam("RMC", "2 1/2", 2.875 * _in, 0.193 * _in),
        Beam("RMC", "3", 3.500 * _in, 0.205 * _in),
        Beam("RMC", "3 1/2", 4.000 * _in, 0.215 * _in),
        Beam("RMC", "4", 4.500 * _in, 0.225 * _in),
    ]}

    _systems = {b.system: 1 for b in beams.values()}
    _nominals = {b.nominal: 1 for b in beams.values()}

    mo.md(
        f"⚙️ Beam systems: {', '.join(_systems.keys())}  \n"
        f"📏 Nominal sizes: {', '.join(_nominals.keys())}"
    )
    return Beam, beams


@app.cell(hide_code=True)
def __(Optional, mo, pint, pydantic_dataclass, units):
    @pydantic_dataclass
    class Material:
        name: str
        strength: Optional[pint.Quantity] = None
        density: Optional[pint.Quantity] = None

    _psi = units.psi
    _g_cm3 = units.gram / units.cm ** 3

    materials = {m.name: m for m in [
        Material("ASTM A53 Grade A", strength=30000 * _psi, density=7.8 * _g_cm3),
        Material("ASTM A53 Grade B", strength=35000 * _psi, density=7.8 * _g_cm3),
        Material("Aluminum 6063-T5", strength=21000 * _psi, density=2.7 * _g_cm3),
    ]}

    mo.md(f"🧱 Materials: {', '.join(materials)}")
    return Material, materials


@app.cell
async def __():
    import fractions
    import marimo as mo
    import math
    import pydantic
    import sys

    if sys.platform == "emscripten":
        import micropip
        await micropip.install("pint")

    import pint

    from typing import List, Optional, Tuple

    units = pint.UnitRegistry()
    units.separate_format_defaults = True
    units.default_format = "#~C"

    _public = {n: v for n, v in globals().items() if not n.startswith("_")}
    _imports = {n for n, v in _public.items() if isinstance(v, type(math))}
    _vars = set(_public) - _imports

    def pydantic_dataclass(*args, config=None, **kwargs):
        config = {"arbitrary_types_allowed": True, **(config or {})}
        return pydantic.dataclasses.dataclass(*args, config=config, **kwargs)

    mo.md(
        f"🐍 Modules: {', '.join(_imports)}  \n"
        f"🌐 Globals: {', '.join(_vars)}\n"
    )
    return (
        List,
        Optional,
        Tuple,
        fractions,
        math,
        micropip,
        mo,
        pint,
        pydantic,
        pydantic_dataclass,
        sys,
        units,
    )


if __name__ == "__main__":
    app.run()
