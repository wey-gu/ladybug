use lbug::{Connection, Database, Error, LogicalType, SystemConfig, Value};

fn database() -> Database {
    Database::in_memory(
        SystemConfig::default()
            .buffer_pool_size(64 * 1024 * 1024)
            .max_db_size(256 * 1024 * 1024),
    )
    .unwrap()
}

fn scalar(
    connection: &Connection<'_>,
    document: Option<&str>,
    key: Option<&str>,
) -> Result<Value, Error> {
    let mut statement = connection.prepare("RETURN json_get_string($document, $key)")?;
    let value = |text: Option<&str>| {
        text.map_or(Value::Null(LogicalType::String), |text| {
            Value::String(text.into())
        })
    };
    let mut output = connection.execute(
        &mut statement,
        vec![("document", value(document)), ("key", value(key))],
    )?;
    let row = output.next().expect("one scalar row");
    assert!(output.next().is_none());
    assert_eq!(row.len(), 1);
    Ok(row.into_iter().next().unwrap())
}

#[test]
fn decoded_top_level_string_uses_last_exact_key_without_numeric_conversion() {
    let database = database();
    let connection = Connection::new(&database).unwrap();
    for (document, expected) in [
        (r#"{"key": "value"}"#, Some("value")),
        (r#"{"key":"\u0076alue"}"#, Some("value")),
        (r#"{"key":"left\u0000right"}"#, Some("left\0right")),
        (
            r#"{"key":"long string before\u0000and after the embedded separator"}"#,
            Some("long string before\0and after the embedded separator"),
        ),
        (r#"{"key":""}"#, Some("")),
        (r#"{"nested":{"key":"value"}}"#, None),
        (r#"{"KEY":"value"}"#, None),
        (r#"{"key":"first","k\u0065y":"last"}"#, Some("last")),
        (r#"{"key":18446744073709551615,"key":"last"}"#, Some("last")),
        (r#"{"key":"first","key":18446744073709551615}"#, None),
        (r#"{"key":"first","key":null}"#, None),
        (r#"{"key":"first","key":false}"#, None),
        (r#"{"key":"first","key":[]}"#, None),
        (r#"{"key":"first","key":{}}"#, None),
        (r#"{"key":123}"#, None),
        (r#"{"key":true}"#, None),
        (r#"{"key":null}"#, None),
        (r#"{"key":[]}"#, None),
        (r#"{"key":{}}"#, None),
        ("{}", None),
        ("[]", None),
        ("null", None),
        ("123", None),
        (r#""value""#, None),
    ] {
        let expected = expected.map_or(Value::Null(LogicalType::String), |text| {
            Value::String(text.into())
        });
        assert_eq!(
            scalar(&connection, Some(document), Some("key")).unwrap(),
            expected,
            "{document}"
        );
    }
    assert_eq!(
        scalar(
            &connection,
            Some(r#"{"key\u0000suffix":"exact","key":"prefix"}"#),
            Some("key\0suffix")
        )
        .unwrap(),
        Value::String("exact".into())
    );
    assert_eq!(
        scalar(&connection, None, Some("key")).unwrap(),
        Value::Null(LogicalType::String)
    );
    assert_eq!(
        scalar(&connection, Some("{}"), None).unwrap(),
        Value::Null(LogicalType::String)
    );
}

#[test]
fn malformed_document_remains_an_error_and_the_connection_recovers() {
    let database = database();
    let connection = Connection::new(&database).unwrap();
    for document in ["{", r#"{"key":"value","other":}"#, r#"{"key":"\uD800"}"#] {
        assert!(scalar(&connection, Some(document), Some("key")).is_err());
    }
    assert_eq!(
        scalar(&connection, Some(r#"{"key":"fresh"}"#), Some("key")).unwrap(),
        Value::String("fresh".into())
    );
}

#[test]
fn selected_rows_keep_nulls_and_flat_key_arguments_aligned() {
    let database = database();
    let connection = Connection::new(&database).unwrap();
    let mut statement = connection
        .prepare(
            "UNWIND $documents AS document WITH document WHERE document <> 'skip' \
         RETURN json_get_string(document, $key)",
        )
        .unwrap();
    let documents = [r#"{"key":"first"}"#, "skip", "{}", r#"{"key":"last"}"#]
        .map(|text| Value::String(text.into()))
        .to_vec();
    let output = connection
        .execute(
            &mut statement,
            vec![
                ("documents", Value::List(LogicalType::String, documents)),
                ("key", Value::String("key".into())),
            ],
        )
        .unwrap()
        .collect::<Vec<_>>();
    assert_eq!(
        output,
        vec![
            vec![Value::String("first".into())],
            vec![Value::Null(LogicalType::String)],
            vec![Value::String("last".into())]
        ]
    );
}

#[test]
fn flat_document_and_nullable_key_vectors_keep_exact_values() {
    let database = database();
    let connection = Connection::new(&database).unwrap();
    let mut statement = connection
        .prepare("UNWIND $keys AS key RETURN json_get_string($document, key)")
        .unwrap();
    let keys = vec![
        Value::String("left".into()),
        Value::Null(LogicalType::String),
        Value::String("right".into()),
        Value::String("missing".into()),
    ];
    let output = connection
        .execute(
            &mut statement,
            vec![
                (
                    "document",
                    Value::String(r#"{"left":"L","right":"R"}"#.into()),
                ),
                ("keys", Value::List(LogicalType::String, keys)),
            ],
        )
        .unwrap()
        .collect::<Vec<_>>();
    assert_eq!(
        output,
        vec![
            vec![Value::String("L".into())],
            vec![Value::Null(LogicalType::String)],
            vec![Value::String("R".into())],
            vec![Value::Null(LogicalType::String)],
        ]
    );
}

#[test]
fn nullable_document_and_key_vectors_share_the_selected_row_positions() {
    let database = database();
    let connection = Connection::new(&database).unwrap();
    let mut statement = connection
        .prepare(
            "UNWIND [0,1,2,3,4] AS i \
         WITH i, CASE WHEN i=3 THEN NULL ELSE CAST($document AS STRING) END AS document, \
              CASE WHEN i=2 THEN NULL WHEN i=1 THEN 'sentinel' \
                   WHEN i=4 THEN 'right' ELSE 'left' END AS key \
         WHERE i<>1 \
         RETURN i AS row_index, json_get_string(document,key) AS selected ORDER BY row_index",
        )
        .unwrap();
    let output = connection
        .execute(
            &mut statement,
            vec![(
                "document",
                Value::String(r#"{"left":"L","right":"R"}"#.into()),
            )],
        )
        .unwrap()
        .collect::<Vec<_>>();
    assert_eq!(
        output,
        vec![
            vec![Value::Int64(0), Value::String("L".into())],
            vec![Value::Int64(2), Value::Null(LogicalType::String)],
            vec![Value::Int64(3), Value::Null(LogicalType::String)],
            vec![Value::Int64(4), Value::String("R".into())],
        ]
    );
}
