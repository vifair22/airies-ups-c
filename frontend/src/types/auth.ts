/* Auth and setup types */

export interface LoginResult {
  token?: string
  error?: string
}

export interface TestResult {
  result?: string
  error?: string
  driver?: string
  topology?: string
  inventory?: {
    model: string
    serial: string
    firmware: string
    nominal_va: number
    nominal_watts: number
  }
}

export interface SetupStatus {
  needs_setup: boolean
  password_set: boolean
  ups_configured: boolean
  ups_connected: boolean
}
