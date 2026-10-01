package dev.mobilewebcambridge.protocol.json

import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertThrows
import org.junit.jupiter.api.Test

class JsonTest {
    @Test
    fun `sorts keys by code point and omits whitespace`() {
        val value = JsonObject(
            linkedMapOf(
                "b" to JsonInt(1),
                "a" to JsonObject(linkedMapOf("d" to JsonBool(true), "c" to JsonNull)),
                "B" to JsonString("x"),
            ),
        )
        assertEquals("""{"B":"x","a":{"c":null,"d":true},"b":1}""", CanonicalJson.write(value))
    }

    @Test
    fun `orders supplementary characters by code point, not UTF-16 unit`() {
        // U+FF5E sorts before U+1F600 by code point, but after it by UTF-16 code unit (0xD83D).
        val value = JsonObject(linkedMapOf("😀" to JsonInt(1), "～" to JsonInt(2)))
        assertEquals("{\"～\":2,\"😀\":1}", CanonicalJson.write(value))
    }

    @Test
    fun `escapes like the other implementations and keeps slashes`() {
        assertEquals("\"a/b \\\"q\\\" \\\\ \\n\\t\\u0001\"", CanonicalJson.write(JsonString("a/b \"q\" \\ \n\t\u0001")))
    }

    @Test
    fun `rejects non-integers`() {
        assertThrows(IllegalArgumentException::class.java) { CanonicalJson.write(JsonDecimal(0.5)) }
    }

    @Test
    fun `parses numbers, escapes and nesting`() {
        val parsed = JsonParser.parse("""{"i":-12,"d":2.5e1,"s":"é\/😀","a":[true,false,null,{}]}""") as JsonObject
        assertEquals(JsonInt(-12), parsed["i"])
        assertEquals(JsonDecimal(25.0), parsed["d"])
        assertEquals(JsonString("é/😀"), parsed["s"])
        assertEquals(JsonArray(listOf(JsonBool(true), JsonBool(false), JsonNull, JsonObject(emptyMap()))), parsed["a"])
    }

    @Test
    fun `rejects malformed documents`() {
        for (text in listOf("", "{", "{\"a\":}", "[1,]", "01", "\"unterminated", "{} x", "tru", "\"\u0001\"")) {
            assertThrows(JsonSyntaxException::class.java, { JsonParser.parse(text) }, text)
        }
    }

    @Test
    fun `bounds nesting depth`() {
        assertThrows(JsonSyntaxException::class.java) { JsonParser.parse("[".repeat(100) + "]".repeat(100)) }
    }
}
