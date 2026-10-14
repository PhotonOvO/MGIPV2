export type EvidenceSource = "renderdoc" | "source" | "runtime" | "build";

export type EvidenceKind =
  | "pixel-history"
  | "draw"
  | "pipeline"
  | "binding"
  | "shader"
  | "constant"
  | "source-code"
  | "build-result"
  | "verification";

export interface Evidence {
  id: string;
  source: EvidenceSource;
  kind: EvidenceKind;
  description: string;
  data: unknown;
}

export interface Hypothesis {
  id: string;
  claim: string;
  evidenceIds: string[];
  expectedBehavior: string;
  confidence: "low" | "medium" | "high";
  verification: string;
  status: "proposed" | "confirmed" | "rejected";
}

function nextIdentifier(prefix: "E" | "H", existing: readonly string[]): string {
  let highest = 0;
  const pattern = new RegExp(`^${prefix}(\\d+)$`);
  for (const id of existing) {
    const match = pattern.exec(id);
    if (match !== null) highest = Math.max(highest, Number(match[1]));
  }
  return `${prefix}${highest + 1}`;
}

export function createEvidence(
  input: Omit<Evidence, "id"> & { id?: string },
  existing: readonly Evidence[] = [],
): Evidence {
  return {
    ...input,
    id: input.id ?? nextIdentifier("E", existing.map((evidence) => evidence.id)),
  };
}

export function createHypothesis(
  input: Omit<Hypothesis, "id"> & { id?: string },
  existing: readonly Hypothesis[] = [],
): Hypothesis {
  return {
    ...input,
    id: input.id ?? nextIdentifier("H", existing.map((hypothesis) => hypothesis.id)),
    evidenceIds: [...input.evidenceIds],
  };
}

export function evidenceById(
  evidence: readonly Evidence[],
  id: string,
): Evidence | undefined {
  return evidence.find((item) => item.id === id);
}
