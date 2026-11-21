from pathlib import Path
import subprocess
import sys
import tempfile

project = Path(__file__).resolve().parents[1]
sample, source, scorer = map(Path, sys.argv[1:])
with tempfile.TemporaryDirectory() as temporary:
    predictions = Path(temporary) / "predictions.jsonl"
    with predictions.open("wb") as stream:
        subprocess.run([project / "selector.exe", sample, source, project / "coverage.json"],
                       stdout=stream, check=True)
    sys.exit(subprocess.call([sys.executable, scorer, sample, sample / "answers.json", predictions]))
