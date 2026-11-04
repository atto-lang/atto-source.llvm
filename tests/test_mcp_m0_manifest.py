"""Source-free native header/contract parity check, not compiler or MCP proof."""
import json
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
CONTRACT = json.loads((ROOT / 'contracts/mcp-dev-control-llvm-m0-v1.json').read_text())
HEADER = (ROOT / 'src/main/cpp/oreslang/runtime/McpDevControlM0.hpp').read_text()

class ManifestParity(unittest.TestCase):
    def test_version_and_disabled_status(self):
        self.assertEqual(CONTRACT['upstreamSchema'], 'atto.mcp.dev-control.m0.v1')
        self.assertEqual(CONTRACT['protocolVersion'], '2026-07-28')
        self.assertFalse(CONTRACT['enabledByDefault'])
        self.assertIn('NOT_GUEST_PARITY', CONTRACT['evidenceScope'])
        for part in ('atto.mcp.dev-control.m0.v1', '2026-07-28'):
            self.assertIn(part, HEADER)
    def test_constants(self):
        names = {'maxFrameBytes':'kFrameBytes', 'maxResponseBytes':'kResponseBytes',
                 'maxJsonDepth':'kJsonDepth', 'maxRequestsPerSecond':'kPerSecond',
                 'maxRequestsPerMinute':'kPerMinute','maxInflight':'kInflight',
                 'maxPrincipalStates':'kPrincipals','maxReplayNonces':'kNonces',
                 'clockSkewMs':'kClockSkewMs','replayWindowMs':'kReplayMs',
                 'deadlineMs':'kDeadlineMs'}
        for key, symbol in names.items():
            match = re.search(r'\b' + re.escape(symbol) + r'\s*=\s*(\d+)\s*;', HEADER)
            self.assertIsNotNone(match, symbol)
            self.assertEqual(int(match.group(1)), CONTRACT['budgets'][key], symbol)
    def test_resource_allowlist(self):
        expected=['process/summary','actors/counters','tasks/counters','scheduler/counters','upgrade/status']
        self.assertEqual([x['suffix'] for x in CONTRACT['resources']], expected)
        self.assertEqual(len(set(expected)), 5)
        for suffix in expected:
            self.assertIn('"atto.mcp.dev-control.m0.v1/'+suffix+'"', HEADER)
        self.assertNotIn('guest/eval', CONTRACT['methods'])
        self.assertNotIn('tools/call', CONTRACT['methods'])
        self.assertNotIn('upgrade/commit', CONTRACT['methods'])

if __name__ == '__main__':
    unittest.main(verbosity=2)
