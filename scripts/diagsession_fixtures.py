#!/usr/bin/env python3
"""Download and independently verify the two pinned native profiling captures.

This is a corpus check, not the application importer. Verification uses an
isolated installation of dissect.etl 3.14 to read ETL framing on Linux.
"""

import argparse
import collections
import hashlib
import json
from pathlib import Path
import shutil
import struct
import tempfile
import urllib.request
import xml.etree.ElementTree as ET
import zipfile


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "tests/fixtures/diagsession/corpus.json"
HEAP_PROVIDER = "222962ab-6180-4b88-a825-346b75f2a24a"
STACK_PROVIDER = "def2fe46-7bd6-4b80-bd94-f57fe20d0ce3"
CPU_PROVIDER = "ce1dbfb4-137e-4da6-87b0-3f59aa102cbc"


def checksum(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def fetch(fixture, directory):
    destination = directory / fixture["filename"]
    if destination.exists() and checksum(destination) == fixture["sha256"]:
        return
    with tempfile.TemporaryDirectory(dir=directory) as temporary:
        download = Path(temporary) / "download"
        with urllib.request.urlopen(fixture["url"], timeout=120) as source:
            with download.open("wb") as output:
                shutil.copyfileobj(source, output)
        if "outer_member" in fixture:
            archive = download
            download = Path(temporary) / "capture"
            with zipfile.ZipFile(archive) as outer:
                with outer.open(fixture["outer_member"]) as source:
                    with download.open("wb") as output:
                        shutil.copyfileobj(source, output)
        if checksum(download) != fixture["sha256"]:
            raise ValueError(f"Checksum mismatch: {fixture['filename']}")
        download.replace(destination)


def snapshot(data):
    """Read only the observed VS 17.8 x64 heapstate v2 fixture layout."""
    assert struct.unpack_from("<4I", data) == (0x407208F6, 2, 8, 0)
    assert struct.unpack_from("<I", data, 52)[0] == 4
    tables = []
    position = 0x240
    for expected_size in (24, 32, 32, 8):
        _, version, size, count = struct.unpack_from("<16sIII", data, position)
        assert version == 1 and size == expected_size
        position += 28
        end = position + size * count
        assert end <= len(data)
        tables.append(data[position:end])
        position = end
    assert position == len(data)
    total_bytes, live_bytes, total_count, live_count = struct.unpack("<QQII", tables[0])
    allocations = list(struct.iter_unpack("<4Q", tables[1]))
    stacks = list(struct.iter_unpack("<IIQQII", tables[2]))
    frames = [value[0] for value in struct.iter_unpack("<Q", tables[3])]
    assert live_count == len(allocations) == sum(row[5] for row in stacks)
    assert live_bytes == sum(row[1] for row in allocations) == sum(row[3] for row in stacks)
    entries = {}
    for qpc, size, address, stack_id in allocations:
        assert stack_id < len(stacks)
        start, depth = stacks[stack_id][:2]
        assert start + depth <= len(frames)
        entries[address, qpc, size] = tuple(frames[start : start + depth])
    assert len(entries) == live_count
    for index, row in enumerate(stacks):
        group = [allocation for allocation in allocations if allocation[3] == index]
        assert len(group) == row[5] and sum(allocation[1] for allocation in group) == row[3]
    return {
        "start_qpc": struct.unpack_from("<Q", data, 24)[0],
        "end_qpc": struct.unpack_from("<Q", data, 32)[0],
        "total_bytes": total_bytes,
        "live_bytes": live_bytes,
        "total_count": total_count,
        "live_count": live_count,
        "entries": entries,
    }


def apply_heap_event(active, event):
    """Replay this fixture's Alloc/Free events and validate its ReAlloc summary.

    Return the allocated identity, or None. A general native reader must also
    handle standalone and failed reallocations; this fixture does not prove them.
    """
    timestamp, pid, tid, opcode, values = event
    heap = values[0]
    if opcode == 33:
        _, size, address, _ = values
        key = pid, heap, address
        assert key not in active, "Allocation overwrites a live address"
        active[key] = timestamp, size, tid
        return key
    if opcode == 36:
        _, address, _ = values
        del active[pid, heap, address]
    elif opcode == 34:
        _, new_address, old_address, new_size, _, _ = values
        # In this capture ReAlloc follows the new Alloc and old Free records.
        # Preserve the original allocation's timestamp and allocating stack.
        assert (pid, heap, old_address) not in active
        allocation = active[pid, heap, new_address]
        assert allocation[0] <= timestamp and allocation[1] == new_size
    else:
        raise ValueError(f"Unsupported heap opcode in fixture: {opcode}")
    return None


def verify_memory(events, stacks, snapshots):
    events.sort(key=lambda event: event[0])
    active = {}
    generations = collections.Counter()
    event_index = 0
    stack_checks = 0
    reports = []
    for name, oracle in sorted(snapshots.items()):
        while event_index < len(events) and events[event_index][0] <= oracle["end_qpc"]:
            key = apply_heap_event(active, events[event_index])
            if key is not None:
                generations[key] += 1
            event_index += 1
        actual = {(address, ts, size) for (_, _, address), (ts, size, _) in active.items()}
        assert actual == oracle["entries"].keys(), f"Outstanding allocation mismatch: {name}"
        for (pid, _, address), (ts, size, tid) in active.items():
            parts = stacks[ts, pid, tid]
            assert len(parts) == 1
            frames = parts[0]
            # All producer snapshots omit the final root frame from the ETL stack.
            assert frames[:-1] == oracle["entries"][address, ts, size]
            stack_checks += 1
        assert len(active) == oracle["live_count"]
        assert sum(value[1] for value in active.values()) == oracle["live_bytes"]
        reports.append({"file": name, **{key: oracle[key] for key in (
            "start_qpc", "end_qpc", "total_bytes", "live_bytes", "total_count", "live_count"
        )}})
    for event in events[event_index:]:
        key = apply_heap_event(active, event)
        if key is not None:
            generations[key] += 1
    return {
        "snapshots": reports,
        "snapshot_allocation_and_stack_checks": stack_checks,
        "reused_addresses": sum(count > 1 for count in generations.values()),
        "maximum_address_generations": max(generations.values()),
        "final_live_count": len(active),
        "final_live_bytes": sum(value[1] for value in active.values()),
    }


def inspect_etl(path, fixture, snapshots):
    # Keep this inspection dependency out of the application and its build.
    from dissect.etl import ETL

    samples = {}
    stacks = collections.defaultdict(list)
    events = []
    headers = collections.Counter()
    opcodes = collections.Counter()
    record_count = 0
    with path.open("rb") as stream:
        etl = ETL(stream)
        assert etl.pointer_size == 8
        logfile = etl.logfile_header._header
        assert logfile.EventsLost == logfile.BufferLost == 0
        buffer_flags = collections.Counter(str(buffer.header.BufferFlag) for buffer in etl.buffers())
        for record in etl:
            header = record.header
            record_count += 1
            headers[type(header).__name__] += 1
            provider = str(header.provider_id)
            opcode = getattr(header, "opcode", None)
            if fixture["kind"] == "cpu" and provider == CPU_PROVIDER and opcode == 46:
                assert header.version == 2
                ip, tid, count, _ = struct.unpack("<QIHH", header.payload)
                key = int(header.time_delta), tid
                assert key not in samples
                samples[key] = ip, count
            elif provider == STACK_PROVIDER and opcode == 32:
                assert header.version == 2
                timestamp, pid, tid = struct.unpack_from("<QII", header.payload)
                if fixture["kind"] == "cpu" or pid == fixture["process_id"]:
                    key = timestamp, pid, tid
                    stacks[key].append(tuple(value[0] for value in struct.iter_unpack("<Q", header.payload[16:])))
            elif provider == HEAP_PROVIDER and opcode in (33, 34, 36):
                assert header.version == 2 and header.process_id == fixture["process_id"]
                formats = {33: "<QQQI", 34: "<QQQQQI", 36: "<QQI"}
                values = struct.unpack(formats[opcode], header.payload)
                events.append((int(header.time_delta), int(header.process_id),
                               int(header.thread_id), int(opcode), values))
                opcodes[opcode] += 1
        report = {
            "etl_records": record_count,
            "header_types": dict(headers),
            "buffer_flags": dict(buffer_flags),
            "pointer_size": etl.pointer_size,
            "qpc_frequency": int(logfile.PerfFreq),
            "events_lost": int(logfile.EventsLost),
            "buffers_lost": int(logfile.BufferLost),
        }
    if fixture["kind"] == "cpu":
        leaf_matches = 0
        for (timestamp, _, tid), parts in stacks.items():
            assert (timestamp, tid) in samples
            leaf_matches += any(samples[timestamp, tid][0] == frames[0] for frames in parts)
        assert leaf_matches > 0
        report.update(cpu_samples=len(samples), correlated_stacks=sum(map(len, stacks.values())),
                      stack_correlation_keys=len(stacks), sampled_ip_matches_first_frame=leaf_matches,
                      target_stacks=sum(len(parts) for (_, pid, _), parts in stacks.items()
                                        if pid == fixture["process_id"]))
    else:
        allocation_stack_count = sum((ts, pid, tid) in stacks for ts, pid, tid, op, _ in events if op in (33, 34))
        assert allocation_stack_count == opcodes[33] + opcodes[34]
        report.update(allocations=opcodes[33], frees=opcodes[36], realloc_summaries=opcodes[34],
                      allocation_stacks=allocation_stack_count)
        report.update(verify_memory(events, stacks, snapshots))
    for key, value in fixture["expected"].items():
        assert report[key] == value, f"{fixture['filename']}: {key} changed"
    return report


def verify(fixture, directory):
    path = directory / fixture["filename"]
    assert path.stat().st_size == fixture["size"]
    assert checksum(path) == fixture["sha256"]
    with zipfile.ZipFile(path) as archive:
        assert archive.testzip() is None
        metadata = ET.fromstring(archive.read("metadata.xml"))
        namespace = {"p": "urn:diagnosticshub-package-metadata-2-1"}
        version = metadata.find("p:Metadata/p:Item[@Key='_BuildVersion']", namespace)
        assert version is not None and version.text == fixture["producer_version"]
        inventory = {entry.filename: entry.file_size for entry in archive.infolist()}
        assert inventory == fixture["resources"]
        snapshots = {Path(name).name: snapshot(archive.read(name)) for name in archive.namelist()
                     if name.endswith(".heapstate")}
        if fixture["kind"] == "memory":
            manifest_path = next(name for name in archive.namelist() if name.endswith(".tmp"))
            manifest = json.loads(archive.read(manifest_path).decode("utf-8-sig"))
            assert manifest["IsNativeEnabled"] and not manifest["IsManagedEnabled"]
            assert len(manifest["Snapshots"]) == len(snapshots) == 6
        etl_path = next(name for name in archive.namelist() if name.endswith(".etl"))
        with tempfile.TemporaryDirectory() as temporary:
            extracted = Path(temporary) / "capture.etl"
            with archive.open(etl_path) as source, extracted.open("wb") as output:
                shutil.copyfileobj(source, output)
            return inspect_etl(extracted, fixture, snapshots)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("fetch", "verify"))
    parser.add_argument("directory", nargs="?", type=Path, default=ROOT / "test_data/diagsession")
    args = parser.parse_args()
    fixtures = json.loads(MANIFEST.read_text())
    args.directory.mkdir(parents=True, exist_ok=True)
    report = {}
    for fixture in fixtures:
        if args.action == "fetch":
            fetch(fixture, args.directory)
        else:
            report[fixture["filename"]] = verify(fixture, args.directory)
        print(f"PASS {args.action}: {fixture['filename']}")
    if report:
        (args.directory / "verification.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
