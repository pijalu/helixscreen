<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>Memory</h2>
      </div>

      <div v-if="loading" class="loading">Loading...</div>
      <div v-else-if="error" class="error">{{ error }}</div>
      <template v-else>
        <!-- Snapshot metrics -->
        <template v-if="data">
          <div class="metrics-row">
            <MetricCard
              title="Avg RSS"
              :value="formatMB(avgRss)"
              subtitle="resident memory"
              color="var(--accent-blue)"
            />
            <MetricCard
              title="Peak VM"
              :value="formatMB(peakVm)"
              subtitle="virtual memory"
              color="var(--accent-red)"
            />
            <MetricCard
              title="P95 RSS"
              :value="formatMB(p95Rss)"
              subtitle="95th percentile"
              color="var(--accent-yellow)"
            />
          </div>

          <div class="chart-section">
            <h3>RSS Over Time</h3>
            <LineChart :data="rssChartData" />
          </div>

          <div class="chart-section">
            <h3>Avg RSS by Platform</h3>
            <BarChart :data="platformChartData" :options="horizontalOpts" />
          </div>
        </template>

        <!-- Memory Warnings section -->
        <template v-if="warnings">
          <div class="section-divider"></div>
          <div class="section-header">
            <h2>Memory Warnings</h2>
            <span class="section-subtitle">Threshold breaches detected by MemoryMonitor</span>
          </div>

          <div v-if="warnings.total_warnings === 0" class="empty-state">
            No memory warnings in this time range.
          </div>
          <template v-else>
            <div class="metrics-row metrics-row-4">
              <MetricCard
                title="Total Warnings"
                :value="String(warnings.total_warnings)"
                subtitle="threshold breaches"
                color="var(--accent-yellow)"
              />
              <MetricCard
                title="Affected Devices"
                :value="String(warnings.affected_devices)"
                subtitle="unique devices"
                color="var(--accent-purple)"
              />
              <MetricCard
                title="Critical"
                :value="String(criticalCount)"
                subtitle="critical level"
                color="var(--accent-red)"
              />
              <MetricCard
                title="Avg RSS at Warning"
                :value="formatMB(avgRssAtWarning)"
                subtitle="when threshold hit"
                color="var(--accent-blue)"
              />
            </div>

            <div class="chart-section">
              <h3>Warnings Over Time</h3>
              <BarChart :data="warningsOverTimeChartData" :options="stackedBarOpts" />
            </div>

            <div class="chart-section">
              <h3>RSS at Warning Time</h3>
              <LineChart :data="rssAtWarningChartData" />
            </div>

            <div class="chart-section">
              <h3>Warnings by Platform</h3>
              <BarChart :data="warningsPlatformChartData" :options="horizontalOpts" />
            </div>

            <div class="chart-section">
              <h3>Recent Warnings</h3>
              <div class="table-container">
                <table class="warnings-table">
                  <thead>
                    <tr>
                      <th>Time</th>
                      <th>Level</th>
                      <th>Device</th>
                      <th>Version</th>
                      <th>Platform</th>
                      <th>RSS</th>
                      <th>Avail</th>
                      <th>Growth/5m</th>
                      <th>Reason</th>
                    </tr>
                  </thead>
                  <tbody>
                    <template v-for="(w, idx) in warnings.recent_warnings" :key="w.timestamp + w.device_id">
                      <tr class="warning-row" :class="{ expanded: expandedRow === idx }" @click="toggleRow(idx)">
                        <td class="mono">{{ formatTimestamp(w.timestamp) }}</td>
                        <td>
                          <span class="level-badge" :class="'level-' + w.level">{{ w.level }}</span>
                        </td>
                        <td class="mono device-id">{{ w.device_id.slice(0, 8) }}</td>
                        <td>{{ w.version }}</td>
                        <td>{{ w.platform }}</td>
                        <td class="mono">{{ formatMB(w.rss_kb) }}</td>
                        <td class="mono">{{ w.system_available_mb }} MB</td>
                        <td class="mono">{{ formatGrowth(w.growth_5min_kb) }}</td>
                        <td class="reason-cell">
                          <span class="expand-icon">{{ expandedRow === idx ? '\u25BC' : '\u25B6' }}</span>
                          {{ w.reason }}
                        </td>
                      </tr>
                      <tr v-if="expandedRow === idx" class="detail-row">
                        <td colspan="9">
                          <div class="detail-grid">
                            <div class="detail-section">
                              <h4>Memory Breakdown</h4>
                              <div class="detail-items">
                                <div class="detail-item">
                                  <span class="detail-label">RSS</span>
                                  <span class="detail-value mono">{{ formatMB(w.rss_kb) }}</span>
                                </div>
                                <div class="detail-item">
                                  <span class="detail-label">PSS</span>
                                  <span class="detail-value mono">{{ formatMB(w.pss_kb) }}</span>
                                </div>
                                <div class="detail-item">
                                  <span class="detail-label">Private Dirty</span>
                                  <span class="detail-value mono">{{ formatMB(w.private_dirty_kb) }}</span>
                                </div>
                                <div class="detail-item">
                                  <span class="detail-label">Growth (5min)</span>
                                  <span class="detail-value mono">{{ formatGrowth(w.growth_5min_kb) }}</span>
                                </div>
                              </div>
                            </div>
                            <div class="detail-section">
                              <h4>System</h4>
                              <div class="detail-items">
                                <div class="detail-item">
                                  <span class="detail-label">Available</span>
                                  <span class="detail-value mono">{{ w.system_available_mb }} MB</span>
                                </div>
                                <div class="detail-item">
                                  <span class="detail-label">Uptime</span>
                                  <span class="detail-value mono">{{ formatUptime(w.uptime_sec) }}</span>
                                </div>
                                <div class="detail-item">
                                  <span class="detail-label">Platform</span>
                                  <span class="detail-value">{{ w.platform }}</span>
                                </div>
                                <div class="detail-item">
                                  <span class="detail-label">Version</span>
                                  <span class="detail-value">{{ w.version }}</span>
                                </div>
                              </div>
                            </div>
                            <div class="detail-section detail-section-full">
                              <h4>Reason</h4>
                              <p class="detail-reason">{{ w.reason }}</p>
                            </div>
                            <div class="detail-section detail-section-full">
                              <h4>Device</h4>
                              <span class="mono">{{ w.device_id }}</span>
                              <button class="filter-device-btn" @click.stop="filterByDevice(w.device_id)">
                                View all data for this device
                              </button>
                            </div>
                          </div>
                        </td>
                      </tr>
                    </template>
                  </tbody>
                </table>
              </div>
            </div>
          </template>
        </template>
      </template>
    </div>
  </AppLayout>
</template>

<script setup lang="ts">
import { ref, watch, computed } from 'vue'
import AppLayout from '@/components/AppLayout.vue'
import MetricCard from '@/components/MetricCard.vue'
import LineChart from '@/components/LineChart.vue'
import BarChart from '@/components/BarChart.vue'
import { useFiltersStore } from '@/stores/filters'
import { api } from '@/services/api'
import type { MemoryData, MemoryWarningsData } from '@/services/api'
import type { ChartOptions } from 'chart.js'

const filters = useFiltersStore()
const data = ref<MemoryData | null>(null)
const warnings = ref<MemoryWarningsData | null>(null)
const loading = ref(true)
const error = ref('')
const expandedRow = ref<number | null>(null)

function toggleRow(idx: number) {
  expandedRow.value = expandedRow.value === idx ? null : idx
}

function filterByDevice(deviceId: string) {
  // Open a new tab with all dashboard data filtered to this device
  // The device_id filter isn't in the standard filter bar, so we'll
  // copy it to clipboard and show a note
  navigator.clipboard.writeText(deviceId)
  alert(`Device ID copied to clipboard:\n${deviceId}\n\nUse this to search telemetry logs for this device.`)
}

const horizontalOpts: ChartOptions<'bar'> = { indexAxis: 'y', scales: { y: { ticks: { autoSkip: false } } } }

const stackedBarOpts: ChartOptions<'bar'> = {
  scales: {
    x: { stacked: true },
    y: { stacked: true }
  }
}

function formatMB(kb: number): string {
  return `${(kb / 1024).toFixed(1)} MB`
}

function formatGrowth(kb: number): string {
  const sign = kb >= 0 ? '+' : ''
  return `${sign}${(kb / 1024).toFixed(1)} MB`
}

function formatTimestamp(ts: string): string {
  const d = new Date(ts)
  return d.toLocaleString(undefined, {
    month: 'short', day: 'numeric',
    hour: '2-digit', minute: '2-digit'
  })
}

function formatUptime(sec: number): string {
  const h = Math.floor(sec / 3600)
  const m = Math.floor((sec % 3600) / 60)
  if (h > 0) return `${h}h ${m}m`
  return `${m}m`
}

// Memory snapshot computeds
const avgRss = computed(() => {
  if (!data.value?.rss_over_time.length) return 0
  const sum = data.value.rss_over_time.reduce((a, d) => a + d.avg_rss_kb, 0)
  return sum / data.value.rss_over_time.length
})

const peakVm = computed(() => {
  if (!data.value?.vm_peak_trend.length) return 0
  return Math.max(...data.value.vm_peak_trend.map(d => d.avg_vm_peak_kb))
})

const p95Rss = computed(() => {
  if (!data.value?.rss_over_time.length) return 0
  return Math.max(...data.value.rss_over_time.map(d => d.p95_rss_kb))
})

const rssChartData = computed(() => ({
  labels: data.value?.rss_over_time.map(d => d.date) ?? [],
  datasets: [
    {
      label: 'Avg RSS (KB)',
      data: data.value?.rss_over_time.map(d => d.avg_rss_kb) ?? [],
      borderColor: '#3b82f6',
      backgroundColor: 'rgba(59, 130, 246, 0.1)',
      fill: true,
      tension: 0.3
    },
    {
      label: 'P95 RSS (KB)',
      data: data.value?.rss_over_time.map(d => d.p95_rss_kb) ?? [],
      borderColor: '#f59e0b',
      backgroundColor: 'rgba(245, 158, 11, 0.1)',
      fill: false,
      tension: 0.3
    }
  ]
}))

const platformChartData = computed(() => ({
  labels: data.value?.rss_by_platform.map(p => p.platform) ?? [],
  datasets: [{
    label: 'Avg RSS (KB)',
    data: data.value?.rss_by_platform.map(p => p.avg_rss_kb) ?? [],
    backgroundColor: '#3b82f6'
  }]
}))

// Memory warnings computeds
const criticalCount = computed(() => {
  if (!warnings.value?.by_level) return 0
  const crit = warnings.value.by_level.find(l => l.level === 'critical')
  return crit?.count ?? 0
})

const avgRssAtWarning = computed(() => {
  if (!warnings.value?.rss_at_warning.length) return 0
  const sum = warnings.value.rss_at_warning.reduce((a, d) => a + d.avg_rss_kb, 0)
  return sum / warnings.value.rss_at_warning.length
})

const LEVEL_COLORS: Record<string, { border: string; bg: string }> = {
  elevated: { border: '#3b82f6', bg: 'rgba(59, 130, 246, 0.7)' },
  warning: { border: '#f59e0b', bg: 'rgba(245, 158, 11, 0.7)' },
  critical: { border: '#ef4444', bg: 'rgba(239, 68, 68, 0.7)' }
}

const warningsOverTimeChartData = computed(() => {
  if (!warnings.value?.over_time.length) return { labels: [], datasets: [] }

  // Get unique sorted dates
  const dates = [...new Set(warnings.value.over_time.map(r => r.date))].sort()

  // Build per-level series
  const levels = ['elevated', 'warning', 'critical']
  const datasets = levels.map(level => {
    const byDate = new Map<string, number>()
    for (const r of warnings.value!.over_time) {
      if (r.level === level) byDate.set(r.date, r.count)
    }
    const colors = LEVEL_COLORS[level] ?? { border: '#888', bg: 'rgba(136,136,136,0.7)' }
    return {
      label: level.charAt(0).toUpperCase() + level.slice(1),
      data: dates.map(d => byDate.get(d) ?? 0),
      borderColor: colors.border,
      backgroundColor: colors.bg
    }
  })

  return { labels: dates, datasets }
})

const rssAtWarningChartData = computed(() => ({
  labels: warnings.value?.rss_at_warning.map(d => d.date) ?? [],
  datasets: [
    {
      label: 'Avg RSS (KB)',
      data: warnings.value?.rss_at_warning.map(d => d.avg_rss_kb) ?? [],
      borderColor: '#3b82f6',
      backgroundColor: 'rgba(59, 130, 246, 0.1)',
      fill: true,
      tension: 0.3
    },
    {
      label: 'Max RSS (KB)',
      data: warnings.value?.rss_at_warning.map(d => d.max_rss_kb) ?? [],
      borderColor: '#ef4444',
      backgroundColor: 'rgba(239, 68, 68, 0.1)',
      fill: false,
      tension: 0.3
    }
  ]
}))

const warningsPlatformChartData = computed(() => ({
  labels: warnings.value?.by_platform.map(p => p.platform) ?? [],
  datasets: [{
    label: 'Warning Count',
    data: warnings.value?.by_platform.map(p => p.count) ?? [],
    backgroundColor: '#f59e0b'
  }]
}))

async function fetchData() {
  loading.value = true
  error.value = ''
  try {
    const [memData, warnData] = await Promise.all([
      api.getMemory(filters.queryString),
      api.getMemoryWarnings(filters.queryString)
    ])
    data.value = memData
    warnings.value = warnData
  } catch (e) {
    error.value = e instanceof Error ? e.message : 'Failed to load data'
  } finally {
    loading.value = false
  }
}

watch(() => filters.queryString, fetchData, { immediate: true })
</script>

<style scoped>
.page-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 24px;
}

.page-header h2 {
  font-size: 20px;
  font-weight: 600;
}

.metrics-row {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 16px;
  margin-bottom: 24px;
}

.metrics-row-4 {
  grid-template-columns: repeat(4, 1fr);
}

.chart-section {
  margin-bottom: 24px;
}

.chart-section h3 {
  font-size: 14px;
  font-weight: 500;
  color: var(--text-secondary);
  margin-bottom: 12px;
}

.section-divider {
  height: 1px;
  background: var(--border);
  margin: 32px 0;
}

.section-header {
  margin-bottom: 24px;
}

.section-header h2 {
  font-size: 20px;
  font-weight: 600;
  margin-bottom: 4px;
}

.section-subtitle {
  font-size: 13px;
  color: var(--text-secondary);
}

.empty-state {
  padding: 40px;
  text-align: center;
  color: var(--text-secondary);
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: 8px;
}

.table-container {
  overflow-x: auto;
  border: 1px solid var(--border);
  border-radius: 8px;
}

.warnings-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 13px;
}

.warnings-table th {
  text-align: left;
  padding: 10px 12px;
  font-weight: 500;
  color: var(--text-secondary);
  background: var(--bg-card);
  border-bottom: 1px solid var(--border);
  white-space: nowrap;
}

.warnings-table td {
  padding: 8px 12px;
  border-bottom: 1px solid var(--border);
  vertical-align: top;
}

.warnings-table tr:last-child td {
  border-bottom: none;
}

.warnings-table tr:hover td {
  background: rgba(59, 130, 246, 0.04);
}

.mono {
  font-family: 'SF Mono', 'Fira Code', 'Cascadia Code', monospace;
  font-size: 12px;
}

.device-id {
  color: var(--text-secondary);
}

.warning-row {
  cursor: pointer;
  transition: background 0.1s;
}

.warning-row.expanded td {
  background: rgba(59, 130, 246, 0.06);
  border-bottom-color: transparent;
}

.expand-icon {
  display: inline-block;
  width: 12px;
  font-size: 10px;
  color: var(--text-secondary);
  margin-right: 4px;
}

.reason-cell {
  max-width: 300px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.detail-row td {
  padding: 0 12px 12px;
  background: rgba(59, 130, 246, 0.03);
}

.detail-grid {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 16px;
  padding: 12px 0;
}

.detail-section {
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 12px;
}

.detail-section-full {
  grid-column: 1 / -1;
}

.detail-section h4 {
  font-size: 11px;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.5px;
  color: var(--text-secondary);
  margin-bottom: 8px;
}

.detail-items {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 6px;
}

.detail-item {
  display: flex;
  justify-content: space-between;
  align-items: baseline;
  font-size: 13px;
}

.detail-label {
  color: var(--text-secondary);
}

.detail-value {
  font-weight: 500;
}

.detail-reason {
  font-size: 13px;
  line-height: 1.5;
  word-break: break-word;
  white-space: normal;
}

.filter-device-btn {
  display: inline-block;
  margin-left: 12px;
  padding: 4px 10px;
  background: transparent;
  border: 1px solid var(--accent-blue);
  border-radius: 4px;
  color: var(--accent-blue);
  font-size: 12px;
  cursor: pointer;
  transition: background 0.15s, color 0.15s;
}

.filter-device-btn:hover {
  background: var(--accent-blue);
  color: #fff;
}

.level-badge {
  display: inline-block;
  padding: 2px 8px;
  border-radius: 4px;
  font-size: 11px;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.5px;
}

.level-elevated {
  background: rgba(59, 130, 246, 0.15);
  color: #60a5fa;
}

.level-warning {
  background: rgba(245, 158, 11, 0.15);
  color: #fbbf24;
}

.level-critical {
  background: rgba(239, 68, 68, 0.15);
  color: #f87171;
}

.loading, .error {
  padding: 40px;
  text-align: center;
  color: var(--text-secondary);
}

.error {
  color: var(--accent-red);
}
</style>
