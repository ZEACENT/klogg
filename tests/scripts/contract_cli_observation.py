"""Quiet-by-default observations for standalone contract CLI invocations."""

import pathlib
import re
import subprocess
import sys


class ContractCliObserver:
    def __init__(self, enabled=False, label="contract-cli"):
        self.enabled = enabled
        self.label = label
        self.sequence = 0

    def __call__(self, command, *, role=None, **kwargs):
        if not self.enabled:
            return subprocess.run(command, **kwargs)
        self.sequence += 1
        sequence = self.sequence
        if role is None:
            role = "inline" if len(command) > 1 and command[1] == "-c" else pathlib.Path(command[1]).name
        role = re.sub(r"[^A-Za-z0-9_.-]", "?", role)[:96]

        def observe(state):
            try:
                print(f"{self.label}[{sequence}] {state}", file=sys.stderr, flush=True)
            except Exception:
                pass

        observe("before role=" + role)
        try:
            completed = subprocess.run(command, **kwargs)
        except subprocess.TimeoutExpired:
            observe("timeout")
            raise
        except BaseException as error:
            observe("error type=" + type(error).__name__)
            raise
        observe(f"returned returncode={completed.returncode}")
        return completed
