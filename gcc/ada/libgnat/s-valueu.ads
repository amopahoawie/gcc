------------------------------------------------------------------------------
--                                                                          --
--                         GNAT COMPILER COMPONENTS                         --
--                                                                          --
--                       S Y S T E M . V A L U E _ U                        --
--                                                                          --
--                                 S p e c                                  --
--                                                                          --
--          Copyright (C) 1992-2025, Free Software Foundation, Inc.         --
--                                                                          --
-- GNAT is free software;  you can  redistribute it  and/or modify it under --
-- terms of the  GNU General Public License as published  by the Free Soft- --
-- ware  Foundation;  either version 3,  or (at your option) any later ver- --
-- sion.  GNAT is distributed in the hope that it will be useful, but WITH- --
-- OUT ANY WARRANTY;  without even the  implied warranty of MERCHANTABILITY --
-- or FITNESS FOR A PARTICULAR PURPOSE.                                     --
--                                                                          --
-- As a special exception under Section 7 of GPL version 3, you are granted --
-- additional permissions described in the GCC Runtime Library Exception,   --
-- version 3.1, as published by the Free Software Foundation.               --
--                                                                          --
-- You should have received a copy of the GNU General Public License and    --
-- a copy of the GCC Runtime Library Exception along with this program;     --
-- see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see    --
-- <http://www.gnu.org/licenses/>.                                          --
--                                                                          --
-- GNAT was originally developed  by the GNAT team at  New York University. --
-- Extensive contributions were provided by Ada Core Technologies Inc.      --
--                                                                          --
------------------------------------------------------------------------------

--  This package contains routines for scanning modular Unsigned
--  values for use in Text_IO.Modular_IO, and the Value attribute.

generic
   type Uns is mod <>;
package System.Value_U is
   pragma Preelaborate;

   procedure Scan_Raw_Unsigned
     (Str : String;
      Ptr : not null access Integer;
      Max : Integer;
      Res : out Uns);
   --  This function scans the string starting at Str (Ptr.all) for a valid
   --  integer according to the syntax described in (RM 3.5(43)). The substring
   --  scanned extends no further than Str (Max).  Note: this does not scan
   --  leading or trailing blanks, nor leading sign.
   --
   --  There are three cases for the return:
   --
   --  If a valid integer is found, then Ptr.all is updated past the last
   --  character of the integer.
   --
   --  If no valid integer is found, then Ptr.all points either to an initial
   --  non-digit character, or to Max + 1 if the field is all spaces and the
   --  exception Constraint_Error is raised.
   --
   --  If a syntactically valid integer is scanned, but the value is out of
   --  range, or, in the based case, the base value is out of range or there
   --  is an out of range digit, then Ptr.all points past the integer, and
   --  Constraint_Error is raised.
   --
   --  Note: these rules correspond to the requirements for leaving the pointer
   --  positioned in Text_IO.Get. Note that the rules as stated in the RM would
   --  seem to imply that for a case like:
   --
   --    8#12345670009#
   --
   --  the pointer should be left at the first # having scanned out the longest
   --  valid integer literal (8), but in fact in this case the pointer points
   --  past the final # and Constraint_Error is raised. This is the behavior
   --  expected for Text_IO and enforced by the ACATS tests.
   --
   --  If a based literal is malformed in that a character other than a valid
   --  hexadecimal digit is encountered during scanning out the digits after
   --  the # (this includes the case of using the wrong terminator, : instead
   --  of # or vice versa) there are two cases. If all the digits before the
   --  non-digit are in range of the base, as in
   --
   --    8#100x00#
   --    8#100:
   --
   --  then in this case, the "base" value before the initial # is returned as
   --  the result, and the pointer points to the initial # character on return.
   --
   --  If an out of range digit has been detected before the invalid character,
   --  as in:
   --
   --   8#900x00#
   --   8#900:
   --
   --  then the pointer is also left at the initial # character, but constraint
   --  error is raised reflecting the encounter of an out of range digit.
   --
   --  Finally if we have an unterminated fixed-point constant where the final
   --  # or : character is missing, Constraint_Error is raised and the pointer
   --  is left pointing past the last digit, as in:
   --
   --   8#22
   --
   --  This string results in a Constraint_Error with the pointer pointing
   --  past the second 2.
   --
   --  Note: If Max is less than Ptr, then Ptr is left unchanged and
   --  Program_Error is raised to indicate that a valid integer cannot
   --  be parsed.
   --
   --  Note: this routine should not be called with Str'Last = Positive'Last.
   --  If this occurs Program_Error is raised with a message noting that this
   --  case is not supported. Most such cases are eliminated by the caller.

   procedure Scan_Unsigned
     (Str : String;
      Ptr : not null access Integer;
      Max : Integer;
      Res : out Uns);
   --  Same as Scan_Raw_Unsigned, except scans optional leading
   --  blanks, and an optional leading plus sign.
   --
   --  Note: if a minus sign is present, Constraint_Error will be raised.
   --  Note: trailing blanks are not scanned.

   function Value_Unsigned (Str : String) return Uns;
   --  Used in computing X'Value (Str) where X is a modular integer type whose
   --  modulus does not exceed the range of System.Unsigned_Types.Unsigned. Str
   --  is the string argument of the attribute. Constraint_Error is raised if
   --  the string is malformed, or if the value is out of range.

end System.Value_U;
