#!/usr/bin/env python3
"""
experiment_tool.py - write FRIDGE001 experiment records for the Smart Fridge app.

Writes what the board cannot know, following the app team's
"Hardware Upload Requirements - Cooked Meals" (raw-data structure v1):

  new-experiment   experiments/<experimentId>/meta            (section 3)
  observe          experiments/<experimentId>/observations/<observationId>
  event            experiments/<experimentId>/events/<eventId>
  status           experiments/<experimentId>/meta/status
  show             prints meta, runs, events, observations and telemetry/FRIDGE001/latest

Usage:
  python experiment_tool.py new-experiment
  python experiment_tool.py observe  --exp COOKED-RICE-001
  python experiment_tool.py event    --exp COOKED-RICE-001
  python experiment_tool.py status   --exp COOKED-RICE-001 completed
  python experiment_tool.py show     --exp COOKED-RICE-001
  python experiment_tool.py --self-test          (offline checks, no network)

It reads FIREBASE_API_KEY, FIREBASE_DATABASE_URL, DEVICE_EMAIL and DEVICE_PASSWORD
from secrets.h (next to this script, else fridge_monitor/secrets.h, or --secrets),
signs in as the device user like the board does, and never prints credentials.
Times are typed in WAT (UTC+1) as YYYY-MM-DD HH:MM and stored as Unix ms.
Only PATCH and PUT to new keys are used; nothing is ever deleted.

Standard library only (Python 3.8+).
"""

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone

DEVICE_ID = "FRIDGE001"
WAT = timezone(timedelta(hours=1))
TIME_FMT = "%Y-%m-%d %H:%M"
LATEST_PATH = "telemetry/FRIDGE001/latest"

OBS_LABELS = ["unknown", "fresh", "spoiled", "inconclusive"]
EVENT_TYPES = ["power_outage", "storage_change", "sample_replaced", "door_opened", "other"]
POWER_KINDS = {"sensor": "sensor_power", "refrigeration": "refrigeration_power"}
STATUSES = ["running", "interrupted", "completed", "abandoned"]
ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,40}$")


class ToolError(Exception):
    pass


# --------------------------------------------------------------------------
# secrets.h and identifiers
# --------------------------------------------------------------------------

def parse_secrets(text):
    """Returns {NAME: value} for every #define NAME "value" in a header."""
    out = {}
    for m in re.finditer(r'^\s*#define\s+(\w+)\s+"((?:[^"\\]|\\.)*)"', text, re.M):
        out[m.group(1)] = re.sub(r"\\(.)", r"\1", m.group(2))
    return out


def looks_placeholder(v):
    u = (v or "").upper()
    return not u or any(s in u for s in ("YOUR", "PLACEHOLDER", "CHANGE", "XXXX", "<"))


def load_secrets(path):
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = [path] if path else [os.path.join(here, "secrets.h"),
                                      os.path.join(here, "fridge_monitor", "secrets.h")]
    for p in candidates:
        if p and os.path.isfile(p):
            with open(p, encoding="utf-8") as f:
                s = parse_secrets(f.read())
            missing = [k for k in ("FIREBASE_API_KEY", "FIREBASE_DATABASE_URL", "DEVICE_EMAIL", "DEVICE_PASSWORD")
                       if looks_placeholder(s.get(k))]
            if missing:
                raise ToolError("%s: missing or placeholder: %s" % (p, ", ".join(missing)))
            return s
    raise ToolError("secrets.h not found (looked in: %s)" % ", ".join(c for c in candidates if c))


def valid_id(s):
    return bool(ID_RE.match(s)) and not s.upper().startswith("CAL-") and s.upper() != "NONE"


# --------------------------------------------------------------------------
# Time (WAT in, Unix ms out)
# --------------------------------------------------------------------------

def wat_to_ms(text):
    dt = datetime.strptime(text.strip(), TIME_FMT).replace(tzinfo=WAT)
    return int(dt.timestamp()) * 1000


def now_ms():
    return int(time.time()) * 1000


def ms_to_wat(ms):
    return datetime.fromtimestamp(ms / 1000, WAT).strftime(TIME_FMT) + " WAT"


# --------------------------------------------------------------------------
# Prompts
# --------------------------------------------------------------------------

def ask(prompt, required=False, default=None, check=None, err="invalid value"):
    hint = " [%s]" % default if default else (" (required)" if required else " (blank = omit)")
    while True:
        v = input("%s%s: " % (prompt, hint)).strip()
        if not v:
            if default is not None:
                return default
            if not required:
                return None
            print("  required")
            continue
        if check and not check(v):
            print("  " + err)
            continue
        return v


def ask_time(prompt, required=False, default_now=False):
    while True:
        v = ask(prompt + " (WAT, YYYY-MM-DD HH:MM%s)" % (", now = current time" if required else ""),
                required=required, default="now" if default_now else None)
        if v is None:
            return None
        if v.lower() == "now":
            return now_ms()
        try:
            return wat_to_ms(v)
        except ValueError:
            print("  use the form 2026-10-15 14:30")


def ask_choice(prompt, choices, default=None):
    return ask("%s (%s)" % (prompt, "/".join(choices)), required=True, default=default,
               check=lambda v: v in choices, err="choose one of: " + ", ".join(choices))


def confirm(prompt):
    return input(prompt + " [y/N]: ").strip().lower() in ("y", "yes")


def ask_exp(args):
    if args.exp:
        if not valid_id(args.exp):
            raise ToolError("invalid experimentId")
        return args.exp
    return ask("experimentId", required=True, check=valid_id,
               err="1-40 chars of A-Z a-z 0-9 - _, not CAL-... or NONE")


# --------------------------------------------------------------------------
# Firebase (Identity Toolkit sign-in + Realtime Database REST)
# --------------------------------------------------------------------------

class Firebase:
    def __init__(self, secrets):
        self.api_key = secrets["FIREBASE_API_KEY"]
        self.db = secrets["FIREBASE_DATABASE_URL"].rstrip("/")
        self._email = secrets["DEVICE_EMAIL"]
        self._password = secrets["DEVICE_PASSWORD"]
        self.token = None
        self.uid = None

    def sign_in(self):
        url = "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=" + \
              urllib.parse.quote(self.api_key)
        body = json.dumps({"email": self._email, "password": self._password,
                           "returnSecureToken": True}).encode()
        req = urllib.request.Request(url, data=body, method="POST",
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                resp = json.load(r)
        except urllib.error.HTTPError as e:
            msg = ""
            try:
                msg = json.loads(e.read().decode()).get("error", {}).get("message", "")
            except Exception:
                pass
            raise ToolError("sign-in failed: HTTP %d %s" % (e.code, msg))
        except urllib.error.URLError as e:
            raise ToolError("sign-in failed: %s" % e.reason)
        self.token = resp["idToken"]
        self.uid = resp.get("localId")
        print("Signed in as the device user (UID %s)" % self.uid)

    def request(self, method, path, data=None, params=None):
        q = {"auth": self.token}
        q.update(params or {})
        url = "%s/%s.json?%s" % (self.db, path.strip("/"), urllib.parse.urlencode(q))
        body = None if data is None else json.dumps(data).encode()
        req = urllib.request.Request(url, data=body, method=method,
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return json.load(r)
        except urllib.error.HTTPError as e:
            detail = e.read().decode(errors="replace")[:200]
            hint = " (database rules do not allow this UID yet?)" if e.code in (401, 403) else ""
            raise ToolError("%s %s -> HTTP %d %s%s" % (method, path, e.code, detail, hint))
        except urllib.error.URLError as e:
            raise ToolError("%s %s failed: %s" % (method, path, e.reason))

    def get(self, path):
        return self.request("GET", path)

    def patch(self, path, data):
        return self.request("PATCH", path, data)

    def put_new(self, path, data):
        """PUT to a key that must not exist yet, so nothing is ever overwritten."""
        if self.get(path) is not None:
            raise ToolError("%s already exists - not overwritten" % path)
        return self.request("PUT", path, data)


def flatten(d, prefix=""):
    """{"food": {"name": x}} -> {"food/name": x}: a PATCH then touches only these leaves."""
    out = {}
    for k, v in d.items():
        p = prefix + k
        if isinstance(v, dict):
            out.update(flatten(v, p + "/"))
        else:
            out[p] = v
    return out


def connect(args):
    fb = Firebase(load_secrets(args.secrets))
    fb.sign_in()
    return fb


# --------------------------------------------------------------------------
# Commands
# --------------------------------------------------------------------------

def build_meta(a):
    """a: answers dict. Optional values that are None are left out, never defaulted."""
    meta = {
        "schemaVersion": 1,
        "experimentId": a["experimentId"],
        "deviceId": DEVICE_ID,
        "sampleId": a["sampleId"],
        "food": {"name": a["food"], "category": "cooked_meal", "preparation": "cooked"},
        "startedAtMs": a["startedAtMs"],
        "purpose": "food_test",
        "status": "running",
        "storage": {"location": a["location"]},
    }
    opt = [("cookedAtMs", meta["food"]), ("recipe", meta["food"]), ("priorHandling", meta["food"]),
           ("container", meta["storage"]), ("cover", meta["storage"])]
    for key, target in opt:
        if a.get(key) is not None:
            target[key] = a[key]
    if a.get("storedAtMs") is not None:
        meta["storedAtMs"] = a["storedAtMs"]
    if a.get("portionAmount") is not None:
        meta["food"]["portion"] = {"amount": a["portionAmount"], "unit": a["portionUnit"]}
    if a.get("notes") is not None:
        meta["notes"] = a["notes"]
    return meta


def is_number(v):
    try:
        float(v)
        return True
    except ValueError:
        return False


def cmd_new_experiment(args):
    print("New cooked-meal experiment. Times in WAT (YYYY-MM-DD HH:MM). Optional fields: blank = omit.")
    a = {}
    a["experimentId"] = ask_exp(args)
    a["sampleId"] = ask("sampleId (the physical meal sample)", required=True, check=valid_id,
                        err="1-40 chars of A-Z a-z 0-9 - _, not CAL-... or NONE")
    a["food"] = ask("food name (e.g. cooked rice)", required=True)
    a["location"] = ask("storage location", required=True)
    a["startedAtMs"] = ask_time("startedAt: when this experiment began recording", required=True)
    a["cookedAtMs"] = ask_time("cookedAt: when cooking completed")
    a["storedAtMs"] = ask_time("storedAt: when the sample entered the storage location")
    a["container"] = ask("container")
    a["cover"] = ask("covered or open", check=lambda v: v in ("covered", "open"), err="covered or open")
    a["recipe"] = ask("recipe / ingredients")
    amount = ask("portion amount (number)", check=is_number, err="a number, e.g. 250")
    if amount is not None:
        a["portionAmount"] = float(amount) if "." in amount else int(amount)
        a["portionUnit"] = ask("portion unit (e.g. g, ml)", required=True)
    a["priorHandling"] = ask("prior storage / reheating history")
    a["notes"] = ask("notes on unknown or uncertain values")
    meta = build_meta(a)

    print(json.dumps(meta, indent=2))
    fb = connect(args)
    path = "experiments/%s/meta" % a["experimentId"]
    try:
        existing = fb.get(path)
    except ToolError as e:
        existing = None
        print("Could not check for an existing meta record: %s" % e)
    if existing is not None:
        print("experiments/%s/meta already exists:" % a["experimentId"])
        print(json.dumps(existing, indent=2))
        if not confirm("Update the fields above in it (other fields are kept)?"):
            return
    elif not confirm("Write this to %s?" % path):
        return
    fb.patch(path, flatten(meta))
    print("Written. On the board: type e, then experimentId %s and sampleId %s." %
          (a["experimentId"], a["sampleId"]))


def latest_run_for(fb, exp):
    try:
        latest = fb.get(LATEST_PATH) or {}
    except ToolError:
        return None
    return latest.get("runId") if latest.get("experimentId") == exp else None


def cmd_observe(args):
    fb = connect(args)
    exp = ask_exp(args)
    meta = fb.get("experiments/%s/meta" % exp) or {}
    obs = {
        "experimentId": exp,
        "sampleId": ask("sampleId", required=True, default=meta.get("sampleId"), check=valid_id),
        "observedAtMs": ask_time("observed at", required=True, default_now=True),
        "label": ask_choice("label", OBS_LABELS),
        "method": ask("method / source", required=True, default="operator_note"),
    }
    notes = ask("notes")
    if notes is not None:
        obs["notes"] = notes
    oid = "OBS-%d" % now_ms()
    print(json.dumps(obs, indent=2))
    if confirm("Write to experiments/%s/observations/%s?" % (exp, oid)):
        fb.put_new("experiments/%s/observations/%s" % (exp, oid), obs)
        print("Written.")


def cmd_event(args):
    fb = connect(args)
    exp = ask_exp(args)
    etype = ask_choice("type", EVENT_TYPES)
    ev = {"experimentId": exp, "type": etype}
    run = ask("runId", required=True, default=latest_run_for(fb, exp),
              check=lambda v: re.match(r"^R\d{5}$", v) is not None, err="like R00012")
    ev["runId"] = run
    ev["observedAtMs"] = ask_time("observed at", required=True, default_now=True)
    if etype == "power_outage":
        ev["power"] = POWER_KINDS[ask_choice("which power", list(POWER_KINDS))]
        start = ask_time("outage start")
        end = ask_time("outage end")
        if start is not None:
            ev["outageStartAtMs"] = start
        if end is not None:
            ev["outageEndAtMs"] = end
    notes = ask("notes", required=True)
    if etype == "power_outage":
        unknown = [n for n, k in (("start", "outageStartAtMs"), ("end", "outageEndAtMs")) if k not in ev]
        if unknown:
            notes += " Outage %s unknown." % " and ".join(unknown)
    ev["notes"] = notes
    ev["source"] = "experiment_tool"
    eid = "EVT-%d" % now_ms()
    print(json.dumps(ev, indent=2))
    if confirm("Write to experiments/%s/events/%s?" % (exp, eid)):
        fb.put_new("experiments/%s/events/%s" % (exp, eid), ev)
        print("Written.")


def cmd_status(args):
    fb = connect(args)
    exp = ask_exp(args)
    status = args.value or ask_choice("status", STATUSES)
    if status not in STATUSES:
        raise ToolError("status must be one of: " + ", ".join(STATUSES))
    path = "experiments/%s/meta" % exp
    if fb.get(path) is None and not confirm("%s does not exist yet. Write status anyway?" % path):
        return
    fb.patch(path, {"status": status})
    print("Status of %s set to %s." % (exp, status))


def cmd_show(args):
    fb = connect(args)
    exp = ask_exp(args)
    base = "experiments/%s" % exp
    for part in ("meta", "runs", "events", "observations"):
        print("\n===== %s/%s =====" % (base, part))
        print(json.dumps(fb.get("%s/%s" % (base, part)), indent=2))
    latest = fb.get(LATEST_PATH)
    print("\n===== %s =====" % LATEST_PATH)
    print(json.dumps(latest, indent=2))
    if latest and latest.get("measuredAtMs"):
        print("measured %s, uploaded %s, quality %s" % (
            ms_to_wat(latest["measuredAtMs"]),
            ms_to_wat(latest["uploadedAtMs"]) if isinstance(latest.get("uploadedAtMs"), int) else "?",
            latest.get("quality", {}).get("state")))
    print("\nRTDB paths:\n  %s/meta\n  %s/runs/<runId>\n  %s/events/<eventId>\n  %s/observations/<observationId>"
          "\n  %s\n  telemetry/FRIDGE001/runs/<runId>/samples/<measurementId>" % (base, base, base, base, LATEST_PATH))


# --------------------------------------------------------------------------
# Self-test (offline)
# --------------------------------------------------------------------------

def self_test():
    s = parse_secrets('#define WIFI_SSID "a b"\n  #define DEVICE_PASSWORD "p\\"q"\n// #x\n#define N 3\n')
    assert s == {"WIFI_SSID": "a b", "DEVICE_PASSWORD": 'p"q'}, s
    assert looks_placeholder("YOUR_API_KEY") and not looks_placeholder("AIza123")
    assert wat_to_ms("2026-10-15 14:30") == 1792071000000, wat_to_ms("2026-10-15 14:30")
    assert ms_to_wat(1792071000000) == "2026-10-15 14:30 WAT"
    assert valid_id("COOKED-RICE-001") and not valid_id("CAL-R00001") and not valid_id("a.b") \
        and not valid_id("NONE") and not valid_id("x" * 41)
    a = {"experimentId": "E1", "sampleId": "S1", "food": "cooked rice", "location": "fridge",
         "startedAtMs": 1, "cookedAtMs": None, "storedAtMs": 2, "container": None, "cover": "open",
         "recipe": None, "priorHandling": None, "notes": None, "portionAmount": 250, "portionUnit": "g"}
    m = build_meta(a)
    assert "cookedAtMs" not in m["food"] and "container" not in m["storage"] and "notes" not in m
    assert m["food"]["category"] == "cooked_meal" and m["food"]["preparation"] == "cooked"
    assert m["purpose"] == "food_test" and m["status"] == "running" and m["schemaVersion"] == 1
    f = flatten(m)
    assert f["food/name"] == "cooked rice" and f["food/portion/unit"] == "g" and f["storage/cover"] == "open"
    print("self-test OK")


def main():
    p = argparse.ArgumentParser(description="Write FRIDGE001 experiment records to Firebase.")
    p.add_argument("command", nargs="?", choices=["new-experiment", "observe", "event", "status", "show"])
    p.add_argument("value", nargs="?", help="status value for the status command")
    p.add_argument("--exp", help="experimentId (prompted if omitted)")
    p.add_argument("--secrets", help="path to secrets.h")
    p.add_argument("--self-test", action="store_true", help="run offline checks and exit")
    args = p.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not args.command:
        p.print_help()
        return 2
    cmds = {"new-experiment": cmd_new_experiment, "observe": cmd_observe, "event": cmd_event,
            "status": cmd_status, "show": cmd_show}
    try:
        cmds[args.command](args)
    except ToolError as e:
        print("ERROR: %s" % e)
        return 1
    except (KeyboardInterrupt, EOFError):
        print("\ncancelled - nothing written")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
