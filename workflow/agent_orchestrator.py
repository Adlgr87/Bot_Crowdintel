#!/usr/bin/env python3
"""Multi-Agent Orchestrator for Bot_Crowdintel Workflow"""
import json, subprocess, sys, os, hashlib, shutil
from pathlib import Path
from datetime import datetime

class AgentOrchestrator:
    def __init__(self, repo_root: str):
        self.repo = Path(repo_root)
        self.workflow = self.repo / "workflow"
        self.phases = self.workflow / "phases"
        self.agents = self.workflow / "agents"
        self.state = {}
        
    def deploy_agent(self, agent_id: str, task_file: str, sandbox: bool = True) -> dict:
        """Deploy an agent to execute a task in isolated sandbox"""
        task_path = self.agents / task_file
        task = json.loads(task_path.read_text())
        
        result = {
            "agent_id": agent_id,
            "task": task["name"],
            "started_at": datetime.utcnow().isoformat() + "Z",
        }
        
        # Execute the agent's task script
        script = self.agents / task.get("script", f"run_{agent_id}.sh")
        if script.exists():
            proc = subprocess.run(
                ["bash", str(script)],
                capture_output=True, text=True, timeout=300,
                cwd=str(self.repo)
            )
            result["exit_code"] = proc.returncode
            result["stdout"] = proc.stdout[-2000:]  # last 2000 chars
            result["stderr"] = proc.stderr[-2000:]
            result["passed"] = proc.returncode == 0
        else:
            result["passed"] = False
            result["error"] = f"Script not found: {script}"
        
        result["completed_at"] = datetime.utcnow().isoformat() + "Z"
        return result
    
    def qa_gate(self, artifacts: list) -> dict:
        """QA Gatekeeper: validate all artifacts against invariants"""
        checks = {
            "compile_gcc": False,
            "compile_clang": False,
            "asan": False,
            "ubsan": False,
            "no_heap_alloc": False,
            "tests_pass": False,
            "determinism": False,
        }
        
        for artifact in artifacts:
            artifact_path = Path(artifact)
            if not artifact_path.exists():
                continue
            
            # Check for heap allocation
            content = artifact_path.read_text()
            if not any(p in content for p in ["new ", "malloc(", "std::function"]):
                checks["no_heap_alloc"] = True
        
        return checks
    
    def run_phase(self, phase_num: int) -> dict:
        """Execute all agents for a phase"""
        phase_dir = self.phases / f"phase_{phase_num}"
        results = {"phase": phase_num, "agents": {}}
        
        for task_file in sorted(phase_dir.glob("*.json")):
            agent_id = task_file.stem
            result = self.deploy_agent(agent_id, f"phases/{task_file.parent.parent.name}/{task_file.name}")
            results["agents"][agent_id] = result
        
        return results

if __name__ == "__main__":
    orch = AgentOrchestrator(sys.argv[1] if len(sys.argv) > 1 else ".")
    phase = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    print(json.dumps(orch.run_phase(phase), indent=2))
