from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
src = repo / (
    "overlay/target/linux/sanechips/base-files/"
    "sbin/e2631-userdata-init"
)

source = src.read_text()

with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    mtd = root / "mtd10"
    ubi = root / "ubi"
    tools = root / "tools"
    log = root / "actions.log"
    mounts = root / "mounts"
    probe = root / "probe"

    for d in (mtd, ubi, tools):
        d.mkdir()

    mounts.write_text("")
    log.write_text("")

    geometry = {
        "name": "userdata",
        "size": "49283072",
        "erasesize": "131072",
        "writesize": "2048",
    }

    for key, value in geometry.items():
        (mtd / key).write_text(value + "\n")

    probe.write_text(
        '#!/bin/sh\n'
        'echo "${E2631_TEST_STATE:-ERASED_CANDIDATE}"\n'
    )
    probe.chmod(0o700)

    replacements = {
        "MTD=/sys/class/mtd/mtd10": f"MTD={mtd}",
        "/sys/class/ubi/": str(ubi) + "/",
        "/proc/mounts": str(mounts),
        "PROBE=/sbin/e2631-userdata-probe": f"PROBE={probe}",
    }

    for name in ("ubiformat", "ubiattach", "ubimkvol"):
        fake = tools / name
        fake.write_text(
            "#!/bin/sh\n"
            f'echo "{name} $*" >> "$E2631_TEST_LOG"\n'
        )
        fake.chmod(0o700)
        replacements[f"/usr/sbin/{name}"] = str(fake)

    script = source
    for old, new in replacements.items():
        assert old in script, f"未找到替换目标: {old}"
        script = script.replace(old, new)

    for name in ("ubiformat", "ubiattach", "ubimkvol"):
        assert f"/usr/sbin/{name}" not in script

    test_script = root / "init.sh"
    test_script.write_text(script)

    env = os.environ.copy()
    env["E2631_TEST_LOG"] = str(log)
    env["E2631_PROBE_BIN"] = str(probe)
    env.pop("E2631_CONFIRM", None)

    def run(mode, **updates):
        e = env.copy()
        e.update(updates)
        return subprocess.run(
            ["sh", str(test_script), mode],
            text=True,
            capture_output=True,
            env=e,
        )

    r = run("--plan")
    assert r.returncode == 0, (r.returncode, r.stdout, r.stderr)
    assert "DRY_RUN_ONLY" in r.stdout
    assert not log.read_text()
    print("PASS: 预览模式不会执行格式化")

    r = run("--apply")
    assert r.returncode == 3
    assert "REFUSED_NO_CONFIRM" in r.stdout
    assert not log.read_text()
    print("PASS: 无确认参数时拒绝执行")

    r = run(
        "--apply",
        E2631_CONFIRM="ERASE_MTD10_USERDATA_47M",
    )
    assert r.returncode == 0, (r.stdout, r.stderr)

    actions = log.read_text().splitlines()
    assert actions == [
        "ubiformat /dev/mtd10 -y",
        "ubiattach /dev/ubi_ctrl -m 10 -d 0",
        "ubimkvol /dev/ubi0 -N userdata -m",
    ], actions

    print("PASS: 模拟写入顺序正确（未调用真实工具）")

    log.write_text("")
    (mtd / "name").write_text("wrong\n")

    r = run(
        "--apply",
        E2631_CONFIRM="ERASE_MTD10_USERDATA_47M",
    )
    assert r.returncode != 0
    assert not log.read_text()
    print("PASS: 错误分区拒绝写入")

    (mtd / "name").write_text("userdata\n")

    r = run(
        "--apply",
        E2631_CONFIRM="ERASE_MTD10_USERDATA_47M",
        E2631_TEST_STATE="NONEMPTY_OR_UNEXPECTED_DATA",
    )
    assert r.returncode != 0
    assert not log.read_text()
    print("PASS: 非空数据拒绝写入")

    (ubi / "ubi0").mkdir()

    r = run(
        "--apply",
        E2631_CONFIRM="ERASE_MTD10_USERDATA_47M",
    )
    assert r.returncode != 0
    assert not log.read_text()
    print("PASS: UBI0 已占用时拒绝写入")

print("PASS: 全部隔离模拟测试通过")
