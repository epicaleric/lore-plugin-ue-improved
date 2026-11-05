//! A tiny gRPC client for the Lore auth service: just the few calls the app needs.
//! Messages are encoded by hand, so no protobuf code generation is needed.

use serde::Serialize;

#[derive(Serialize, Clone, Default, Debug)]
#[serde(rename_all = "camelCase")]
pub struct UserToken {
    pub token: String,
    pub expires_at: i64,
    pub user_id: String,
    pub user_name: String,
}

fn put_varint(mut value: u64, out: &mut Vec<u8>) {
    loop {
        let byte = (value & 0x7f) as u8;
        value >>= 7;
        if value == 0 {
            out.push(byte);
            break;
        }
        out.push(byte | 0x80);
    }
}

fn put_string(field: u32, text: &str, out: &mut Vec<u8>) {
    put_varint(((field << 3) | 2) as u64, out);
    put_varint(text.len() as u64, out);
    out.extend_from_slice(text.as_bytes());
}

enum Field {
    Varint(u64),
    Bytes(Vec<u8>),
}

fn read_varint(buf: &[u8], pos: &mut usize) -> Option<u64> {
    let mut value = 0u64;
    let mut shift = 0;
    while *pos < buf.len() {
        let byte = buf[*pos];
        *pos += 1;
        value |= ((byte & 0x7f) as u64) << shift;
        if byte & 0x80 == 0 {
            return Some(value);
        }
        shift += 7;
        if shift > 63 {
            return None;
        }
    }
    None
}

fn parse(buf: &[u8]) -> Vec<(u32, Field)> {
    let mut fields = Vec::new();
    let mut pos = 0;
    while pos < buf.len() {
        let Some(tag) = read_varint(buf, &mut pos) else { break };
        let number = (tag >> 3) as u32;
        match tag & 7 {
            0 => {
                let Some(value) = read_varint(buf, &mut pos) else { break };
                fields.push((number, Field::Varint(value)));
            }
            2 => {
                let Some(len) = read_varint(buf, &mut pos) else { break };
                let end = pos + len as usize;
                if end > buf.len() {
                    break;
                }
                fields.push((number, Field::Bytes(buf[pos..end].to_vec())));
                pos = end;
            }
            1 => pos += 8,
            5 => pos += 4,
            _ => break,
        }
    }
    fields
}

fn string_field(fields: &[(u32, Field)], number: u32) -> String {
    for (n, field) in fields {
        if *n == number {
            if let Field::Bytes(bytes) = field {
                return String::from_utf8_lossy(bytes).to_string();
            }
        }
    }
    String::new()
}

fn user_token(bytes: &[u8]) -> UserToken {
    let fields = parse(bytes);
    let mut expires_at = 0i64;
    for (n, field) in &fields {
        if *n == 2 {
            if let Field::Varint(value) = field {
                expires_at = *value as i64;
            }
        }
    }
    UserToken {
        token: string_field(&fields, 1),
        expires_at,
        user_id: string_field(&fields, 3),
        user_name: string_field(&fields, 4),
    }
}

async fn call(auth_url: &str, method: &str, message: Vec<u8>) -> Result<Vec<u8>, String> {
    let client = reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(20))
        .build()
        .map_err(|e| e.to_string())?;

    let mut body = Vec::with_capacity(message.len() + 5);
    body.push(0);
    body.extend_from_slice(&(message.len() as u32).to_be_bytes());
    body.extend_from_slice(&message);

    let url = format!("{}/epic_urc.UrcAuthApi/{}", auth_url.trim_end_matches('/'), method);
    let response = client
        .post(url)
        .header("content-type", "application/grpc")
        .header("te", "trailers")
        .body(body)
        .send()
        .await
        .map_err(|e| format!("Could not reach the authentication service: {e}"))?;

    let status = response
        .headers()
        .get("grpc-status")
        .and_then(|v| v.to_str().ok())
        .map(|s| s.to_string());
    let message_text = response
        .headers()
        .get("grpc-message")
        .and_then(|v| v.to_str().ok())
        .map(|s| s.to_string())
        .unwrap_or_default();
    let bytes = response.bytes().await.map_err(|e| e.to_string())?;
    if let Some(status) = status {
        if status != "0" {
            return Err(format!("The authentication service answered {status}: {message_text}"));
        }
    }
    if bytes.len() < 5 {
        return Err("Empty answer from the authentication service".into());
    }
    Ok(bytes[5..].to_vec())
}

pub async fn start_session(auth_url: &str, client_state: &str) -> Result<String, String> {
    let mut message = Vec::new();
    put_string(1, client_state, &mut message);
    let reply = call(auth_url, "StartAuthSession", message).await?;
    Ok(string_field(&parse(&reply), 1))
}

pub async fn get_session(auth_url: &str, session: &str, client_state: &str) -> Result<UserToken, String> {
    let mut message = Vec::new();
    put_string(1, session, &mut message);
    put_string(2, client_state, &mut message);
    let reply = call(auth_url, "GetAuthSession", message).await?;
    let fields = parse(&reply);
    for (n, field) in &fields {
        if *n == 1 {
            if let Field::Bytes(bytes) = field {
                return Ok(user_token(bytes));
            }
        }
    }
    Err("The sign-in was not completed".into())
}

pub async fn exchange_api_key(auth_url: &str, key: &str) -> Result<UserToken, String> {
    let mut message = Vec::new();
    put_string(1, key, &mut message);
    let reply = call(auth_url, "ExchangeAPIKeyForUserToken", message).await?;
    let fields = parse(&reply);
    for (n, field) in &fields {
        if *n == 1 {
            if let Field::Bytes(bytes) = field {
                return Ok(user_token(bytes));
            }
        }
    }
    Err("No token in the answer".into())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn encodes_and_parses_strings() {
        let mut out = Vec::new();
        put_string(1, "hello", &mut out);
        put_string(2, "wörld", &mut out);
        let fields = parse(&out);
        assert_eq!(string_field(&fields, 1), "hello");
        assert_eq!(string_field(&fields, 2), "wörld");
    }

    #[test]
    fn parses_a_user_token() {
        let mut token = Vec::new();
        put_string(1, "abc.def", &mut token);
        put_varint((2 << 3) as u64, &mut token);
        put_varint(1_800_000_000, &mut token);
        put_string(3, "id-1", &mut token);
        put_string(4, "Eric", &mut token);
        let parsed = user_token(&token);
        assert_eq!(parsed.token, "abc.def");
        assert_eq!(parsed.expires_at, 1_800_000_000);
        assert_eq!(parsed.user_name, "Eric");
    }

    /// Talks to the real service, but needs no login: it only opens a sign-in session.
    #[tokio::test]
    async fn starts_a_session_on_the_live_auth_service() {
        let url = std::env::var("LORE_TEST_AUTH_URL").unwrap_or_default();
        if url.is_empty() {
            return;
        }
        let session = start_session(&url, "test-state-1234").await.expect("session");
        assert!(!session.is_empty());
    }
}