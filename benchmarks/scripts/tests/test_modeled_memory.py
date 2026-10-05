import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('memory_runner',Path(__file__).resolve().parents[1]/'run_modeled_memory.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)


class CalibrationTests(unittest.TestCase):
    def model(self, metadata, cost=lambda c:0):
        calls=[]
        def replay(capacity,phase):
            calls.append((capacity,phase))
            return dict(capacity_bytes=capacity,metadata_bytes_peak=metadata(capacity,phase),total_miss_cost_us=cost(capacity))
        return replay,calls

    def test_constant_metadata_exact_budget_and_verification(self):
        replay,calls=self.model(lambda c,p:100)
        result=m.calibrate(1000,500,replay)
        self.assertEqual(result['selected']['capacity'],900)
        self.assertEqual(calls[-1],(900,'verification'))
        self.assertEqual(result['status'],'verified_within_1pct')
        self.assertLessEqual(len(calls),6)

    def test_cost_cannot_influence_capacity(self):
        a,_=self.model(lambda c,p:c//3,lambda c:c)
        b,_=self.model(lambda c,p:c//3,lambda c:10**12-c)
        x=m.calibrate(1000,500,a);y=m.calibrate(1000,500,b)
        self.assertEqual([r['capacity'] for r in x['attempts']],[r['capacity'] for r in y['attempts']])
        self.assertEqual(x['selected']['capacity'],y['selected']['capacity'])

    def test_nonmonotonic_allocator_retains_largest_observed_feasible(self):
        replay,calls=self.model(lambda c,p:50 if 700<=c<=800 else 400)
        result=m.calibrate(1000,750,replay)
        feasible=[a['capacity'] for a in result['attempts'] if a['slack']>=0]
        self.assertEqual(result['selected']['capacity'],max(feasible))
        self.assertLessEqual(len(calls),6)
        self.assertGreater(result['verification']['slack'],10)
        self.assertEqual(result['status'],'verified_slack_above_1pct')

    def test_verification_can_fail_and_is_not_recalibrated(self):
        replay,calls=self.model(lambda c,p:200 if p=='verification' else 100)
        result=m.calibrate(1000,900,replay)
        self.assertEqual(result['status'],'verification_infeasible')
        self.assertEqual(len(calls),2)

    def test_no_feasible_result_is_explicit(self):
        replay,calls=self.model(lambda c,p:1001)
        result=m.calibrate(1000,500,replay)
        self.assertEqual(result['status'],'no_observed_feasible_capacity')
        self.assertIsNone(result['selected'])
        self.assertLessEqual(len(calls),5)

    def test_cached_reference_counts_toward_budget(self):
        replay,calls=self.model(lambda c,p:100)
        initial=dict(capacity_bytes=900,metadata_bytes_peak=100)
        result=m.calibrate(1000,900,replay,initial=initial)
        self.assertEqual(calls,[(900,'verification')])
        self.assertEqual(len(result['attempts']),1)


if __name__=='__main__':unittest.main()
