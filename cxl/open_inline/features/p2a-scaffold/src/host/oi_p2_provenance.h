/* oi_p2_provenance.h — schema reference for P2-R12 port-provenance records.
 *
 * The actual provenance data lives in a checked-in JSON file, one entry per kernel source, at
 * src/kernels/provenance.json (sibling to the .cl sources it describes). This header exists so
 * any C/C++ tooling that wants to assert against the schema version has one place to include,
 * and so the schema is documented next to the code it governs, not only in Python.
 *
 * Schema ("oi-p2-provenance/1"), one array entry per kernel file:
 *   {
 *     "kernel":       "k5_descrambler",              // source file stem under src/kernels/
 *     "type":         "port" | "fresh",               // T3 port vs T4 fresh (classification.md)
 *     "ocudu": {
 *       "repo":  "https://gitlab.com/ocudu/ocudu",
 *       "tag":   "release_26_04",
 *       "sha":   "<40-hex clone commit SHA>"
 *     },
 *     "port_sources": ["lib/phy/upper/.../foo_impl.cpp", ...],  // required if type == "port"
 *     "references":   ["include/ocudu/.../bar.h", ...]          // required if type == "fresh"
 *                                                                 // (read-only field-semantics
 *                                                                 // sources; K1's case)
 *   }
 *
 * provenance_check.py (P2-R12) fails CI if any src/kernels/k*.cl file has no matching entry, or
 * if an entry is missing port_sources (type=="port") / references (type=="fresh").
 */
#ifndef OI_P2_PROVENANCE_H
#define OI_P2_PROVENANCE_H

#define OI_P2_PROVENANCE_SCHEMA "oi-p2-provenance/1"

#endif /* OI_P2_PROVENANCE_H */
