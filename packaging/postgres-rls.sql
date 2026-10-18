-- Row-level security for the sync tables.
-- Each connecting role may only touch the tenant it announces for itself.
--
-- Caveats worth knowing before applying to production:
--   * table owners and roles with BYPASSRLS (superusers) are not restricted;
--   * the client must run SET tf.tenant = '<its tenant>' on every connection,
--     otherwise the policy matches nothing and reads come back empty;
--   * FORCE ROW LEVEL SECURITY is what also binds the table owner.

alter table teleforge_messages   enable row level security;
alter table teleforge_messages   force  row level security;
alter table teleforge_events     enable row level security;
alter table teleforge_events     force  row level security;
alter table teleforge_embeddings enable row level security;
alter table teleforge_embeddings force  row level security;
alter table teleforge_sync_state enable row level security;
alter table teleforge_sync_state force  row level security;

drop policy if exists tenant_isolation on teleforge_messages;
create policy tenant_isolation on teleforge_messages
	using (tenant_id = current_setting('tf.tenant', true))
	with check (tenant_id = current_setting('tf.tenant', true));

drop policy if exists tenant_isolation on teleforge_events;
create policy tenant_isolation on teleforge_events
	using (tenant_id = current_setting('tf.tenant', true))
	with check (tenant_id = current_setting('tf.tenant', true));

drop policy if exists tenant_isolation on teleforge_embeddings;
create policy tenant_isolation on teleforge_embeddings
	using (tenant_id = current_setting('tf.tenant', true))
	with check (tenant_id = current_setting('tf.tenant', true));

drop policy if exists tenant_isolation on teleforge_sync_state;
create policy tenant_isolation on teleforge_sync_state
	using (tenant_id = current_setting('tf.tenant', true))
	with check (tenant_id = current_setting('tf.tenant', true));
