import tempfile
import unittest
from pathlib import Path

from tools.drum_research.analyze_drum_groove_corpora import analyze


class DrumGrooveCorporaAnalysisTests(unittest.TestCase):
    def test_reports_timing_limb_offsets_and_sequence_presence(self) -> None:
        text = (
            '"","Corpus","Drummer","Track","Year","Strike","Instrument",'
            '"MetricTime","OnsetTime","MetronomicOnsetTime","Tempo",'
            '"MicrotimingSeconds","MicrotimingBeats","TrackDuration"\n'
            '"1","Loop","A","one.wav",NA,1,"BD",0,0,0,120,0,0,4\n'
            '"2","Loop","A","one.wav",NA,2,"HH",0,0.005,0,120,0.005,0.01,4\n'
            '"3","Loop","A","one.wav",NA,3,"SD",1,0.49,0.5,120,-0.01,-0.02,4\n'
            '"4","Loop","A","one.wav",NA,4,"BD",4,2,2,120,0,0,4\n'
            '"5","Loop","A","one.wav",NA,5,"HH",4,2.005,2,120,0.005,0.01,4\n'
            '"6","Loop","A","one.wav",NA,6,"SD",5,2.49,2.5,120,-0.01,-0.02,4\n'
        )
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "dgc.csv"
            path.write_text(text, encoding="utf-8")
            result = analyze(path)
        loop = result["byCorpus"]["Loop"]
        self.assertEqual(result["source"]["rows"], 6)
        self.assertEqual(loop["instrumentCounts"]["HH"], 2)
        self.assertAlmostEqual(
            loop["coincidentLimbOffsetMs"]["HH-minus-BD"]["median"],
            5.0,
        )
        self.assertEqual(
            loop["sequence"]["adjacentPresenceSimilarity"]["median"],
            1.0,
        )


if __name__ == "__main__":
    unittest.main()
