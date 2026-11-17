"""Python tooling for the Market Data Simulator.

Pure standard library by design: everything here works on a stock Python 3.9+
install. pandas / matplotlib are used opportunistically when present (see
``requirements.txt``) but are never required.

Modules
-------
codec       MarketEvent binary/CSV codec, byte-compatible with the C++ struct.
resp        Minimal RESP2 Redis client with pipelining.
store       Read-side mirror of the C++ TickStore key schema.
book        Small reference order book used to cross-check C++ output.
analysis    Backtest report / equity-curve / fills analytics.
"""

__version__ = "1.0.0"
