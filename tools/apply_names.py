#!/usr/bin/env python3
"""Apply an {address: name} map to xboxrecomp's functions.json in place.

Optional third argument: a JSON list of extra function entries,
{"start": "0x...", "end": "0x...", "why": "..."}, for code the disassembler
did not split out as a function but something reaches by address (see
names/extra_functions.json). Entries already present are left alone.
"""
import json, sys
fp, np = sys.argv[1], sys.argv[2]
funcs = json.load(open(fp)); names = json.load(open(np)); k = 0
for f in funcs:
    a = "0x%08X" % int(f["start"], 16)
    if a in names:
        f["name"] = names[a]; k += 1
added = 0
if len(sys.argv) > 3:
    have = {int(f["start"], 16) for f in funcs}
    for e in json.load(open(sys.argv[3])):
        s, end = int(e["start"], 16), int(e["end"], 16)
        if s in have:
            continue
        funcs.append({
            "start": "0x%08X" % s, "end": "0x%08X" % end, "size": end - s,
            "name": names.get("0x%08X" % s, "sub_%08X" % s),
            "section": e.get("section", ".text"), "confidence": 0.9,
            "detection_method": "manual", "num_instructions": 0,
            "has_prologue": False, "calls_to": [], "called_by": [],
        })
        added += 1
    funcs.sort(key=lambda f: int(f["start"], 16))
json.dump(funcs, open(fp, "w"))
print(f"applied {k} names, added {added} functions")
