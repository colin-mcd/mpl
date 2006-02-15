signature WORD_GLOBAL =
   sig
      eqtype word
   end

signature PRE_WORD =
   sig
      include WORD_GLOBAL

      val toLarge: word -> LargeWord.word
      val toLargeX: word -> LargeWord.word
      val toLargeWord: word -> LargeWord.word
      val toLargeWordX: word -> LargeWord.word
      val fromLarge: LargeWord.word -> word
      val fromLargeWord: LargeWord.word -> word
      val toLargeInt: word -> LargeInt.int
      val toLargeIntX: word -> LargeInt.int
      val fromLargeInt: LargeInt.int -> word
      val toInt: word -> int
      val toIntX: word -> int
      val fromInt: int -> word
         
      val andb: word * word -> word
      val orb: word * word -> word
      val xorb: word * word -> word
      val notb: word -> word
         
      val + : word * word -> word
      val - : word * word -> word
      val * : word * word -> word
      val div: word * word -> word
      val mod: word * word -> word
         
      val compare: word * word -> order
      val < : word * word -> bool
      val <= : word * word -> bool
      val > : word * word -> bool
      val >= : word * word -> bool
         
      val ~ : word -> word
      val min: word * word -> word
      val max: word * word -> word
   end
signature PRE_WORD_EXTRA =
   sig
      include PRE_WORD

      val zero: word

      val wordSize: Primitive.Int32.int

      val << : word * Primitive.Word32.word -> word
      val >> : word * Primitive.Word32.word -> word
      val ~>> : word * Primitive.Word32.word -> word
      val rol: word * Primitive.Word32.word -> word
      val ror: word * Primitive.Word32.word -> word
   end

signature WORD =
   sig
      include PRE_WORD

      val wordSize: Int.int

(*
      val << : word * Word.word -> word
      val >> : word * Word.word -> word
      val ~>> : word * Word.word -> word
*)
         
      val fmt: StringCvt.radix -> word -> string
      val toString: word -> string
      val scan: (StringCvt.radix
                 -> (char, 'a) StringCvt.reader
                 -> (word, 'a) StringCvt.reader)
      val fromString: string -> word option
   end

signature WORD_EXTRA =
   sig
      include WORD

      val rol: word * Word.word -> word
      val ror: word * Word.word -> word
   end
