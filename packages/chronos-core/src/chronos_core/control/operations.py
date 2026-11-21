"""Workspace versioning operations, defined once for every Chronos front end.

Each function calls the workspace (or one named store's) Python API. The CLI
commands and the gateway's control routes both call these functions, so the two
front ends behave identically. Policies are API values (None for the API's
default).
"""
from __future__ import annotations

from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from chronos_core.workspace import ChronosWorkspaceContext


def list_branches(workspace: ChronosWorkspaceContext, store: str | None = None) -> Any:
    """Branch names across the workspace, or one store's branch records with metadata."""
    return workspace.list_branches() if store is None else workspace.stores[store].list_branches()


def create_branch(workspace: ChronosWorkspaceContext, name: str, from_branch: str = "main",
                  metadata: dict[str, Any] | None = None) -> Any:
    return workspace.create_branch(name, from_branch=from_branch, metadata=metadata)


def create_branch_from_checkpoint(workspace: ChronosWorkspaceContext, name: str, checkpoint: str) -> Any:
    return workspace.create_branch_from_checkpoint(name, checkpoint)


def update_branch_metadata(workspace: ChronosWorkspaceContext, store: str, name: str,
                           metadata: dict[str, Any]) -> Any:
    return workspace.stores[store].update_branch_metadata(name, metadata)


def delete_branch(workspace: ChronosWorkspaceContext, name: str) -> Any:
    return workspace.delete_branch(name)


def create_checkpoint(workspace: ChronosWorkspaceContext, name: str, branch: str = "main",
                      metadata: dict[str, Any] | None = None) -> Any:
    return workspace.create_checkpoint(name, branch=branch, metadata=metadata)


def list_checkpoints(workspace: ChronosWorkspaceContext, store: str, branch: str | None = None,
                     metadata_filter: dict[str, Any] | None = None) -> Any:
    return workspace.stores[store].list_checkpoints(branch, metadata_filter)


def diff(workspace: ChronosWorkspaceContext, left: str, right: str) -> Any:
    return workspace.diff(left, right)


def merge_preview(workspace: ChronosWorkspaceContext, source: str, target: str, policy: str | None) -> Any:
    return workspace.merge_preview(source, target, policy=policy)


def merge_apply(workspace: ChronosWorkspaceContext, source: str, target: str, policy: str | None,
                keep_branch: bool = False) -> Any:
    """Merge source into target; keep_branch then continues source from the merged target."""
    if keep_branch:
        return workspace.merge_apply(source, target, policy=policy, keep_branch=True)
    return workspace.merge_apply(source, target, policy=policy)


def merge_atomic_preview(workspace: ChronosWorkspaceContext, source: str, target: str,
                         policy: str | None) -> Any:
    return workspace.merge_atomic_preview(source, target, policy=policy)


def merge_atomic(workspace: ChronosWorkspaceContext, source: str, target: str, *, operation_id: str,
                 preview_token: str | None = None, policy: str | None = None,
                 keep_branch: bool = False) -> Any:
    """Merge source into target atomically; keep_branch then continues source from the merged target."""
    options: dict[str, Any] = {"keep_branch": True} if keep_branch else {}
    return workspace.merge_atomic(
        source, target, operation_id=operation_id, preview_token=preview_token, policy=policy, **options,
    )


def create_table(workspace: ChronosWorkspaceContext, store: str, sql: str) -> Any:
    """Run one ordinary CREATE TABLE on the store, as `chronos table create` does."""
    database = workspace.stores[store]
    database.db.execute(sql)
    return database.db.commit()


def register_table(workspace: ChronosWorkspaceContext, store: str, table: str, primary_key: list[str]) -> Any:
    return workspace.stores[store].register_table(table, primary_key)


def create_index(workspace: ChronosWorkspaceContext, store: str, table: str, columns: list[str],
                 name: str | None = None) -> Any:
    return workspace.stores[store].create_index(table, columns, name)


def table_info(workspace: ChronosWorkspaceContext, store: str, table: str) -> Any:
    return workspace.stores[store].describe_table(table)
